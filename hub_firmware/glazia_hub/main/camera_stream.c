#include "camera_stream.h"

#include <stdbool.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "state.h"

#define CAM_PWDN    -1
#define CAM_RESET   -1
#define CAM_XCLK    15
#define CAM_SIOD     4
#define CAM_SIOC     5
#define CAM_VSYNC    6
#define CAM_HREF     7
#define CAM_PCLK    13
#define CAM_D0      11
#define CAM_D1       9
#define CAM_D2       8
#define CAM_D3      10
#define CAM_D4      12
#define CAM_D5      18
#define CAM_D6      17
#define CAM_D7      16

/* OV5640 XCLK: 16 MHz intentional — 20 MHz causes NO-EOI/frame-timeout in
 * PSRAM-DMA mode at QVGA (PCLK ~32 MHz at 16 MHz XCLK still fits DMA window). */
#define CAM_XCLK_HZ    16000000

/* fb_count=1 + GRAB_WHEN_EMPTY: halves DMA descriptor ring, eliminates
 * mid-write overwrite race that caused NO-EOI with fb_count=2+GRAB_LATEST.
 * Trade-off: sensor stalls between frames. Acceptable at ≤15fps. */
#define CAM_FB_COUNT    1
#define CAM_GRAB_MODE   CAMERA_GRAB_WHEN_EMPTY

#define JPEG_QUALITY    12

static const char *TAG = "CAMERA";

static bool s_camera_ready;

static void aec_set_center_weighted(sensor_t *s)
{
    s->set_reg(s, 0x5688, 0xff, 0x12);
    s->set_reg(s, 0x5689, 0xff, 0x21);
    s->set_reg(s, 0x568A, 0xff, 0x28);
    s->set_reg(s, 0x568B, 0xff, 0x82);
    s->set_reg(s, 0x568C, 0xff, 0x28);
    s->set_reg(s, 0x568D, 0xff, 0x82);
    s->set_reg(s, 0x568E, 0xff, 0x12);
    s->set_reg(s, 0x568F, 0xff, 0x21);
    ESP_LOGI(TAG, "AEC center-weighted metering applied");
}

esp_err_t camera_ensure_init(void)
{
    if (s_camera_ready) return ESP_OK;

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "Camera init: internal_free=%u internal_largest=%u psram_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t err = esp_camera_set_psram_mode(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_set_psram_mode failed: %s", esp_err_to_name(err));
    }

    camera_config_t cfg = {
        .pin_pwdn     = CAM_PWDN,
        .pin_reset    = CAM_RESET,
        .pin_xclk     = CAM_XCLK,
        .pin_sccb_sda = CAM_SIOD,
        .pin_sccb_scl = CAM_SIOC,
        .pin_vsync    = CAM_VSYNC,
        .pin_href     = CAM_HREF,
        .pin_pclk     = CAM_PCLK,
        .pin_d0       = CAM_D0,
        .pin_d1       = CAM_D1,
        .pin_d2       = CAM_D2,
        .pin_d3       = CAM_D3,
        .pin_d4       = CAM_D4,
        .pin_d5       = CAM_D5,
        .pin_d6       = CAM_D6,
        .pin_d7       = CAM_D7,
        .xclk_freq_hz = CAM_XCLK_HZ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        /* YUV422 for H.264 encoder input — raw sensor data, no JPEG compression. */
        .pixel_format = PIXFORMAT_YUV422,
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = JPEG_QUALITY,
        .fb_count     = CAM_FB_COUNT,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAM_GRAB_MODE,
    };

    err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "esp_camera_sensor_get failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor PID=0x%04x (OV5640 expected: 0x5640)", s->id.PID);

    s->set_brightness(s,    2);
    s->set_contrast(s,      1);
    s->set_saturation(s,    2);
    s->set_sharpness(s,     1);
    s->set_whitebal(s,      1);
    s->set_awb_gain(s,      1);
    s->set_hmirror(s,       1);
    s->set_vflip(s,         0);

    aec_set_center_weighted(s);

    s->set_gain_ctrl(s,     1);
    s->set_gainceiling(s,   GAINCEILING_64X);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s,          1);
    s->set_ae_level(s,      2);

    /* OV5640 AEC/AWB needs several VSYNC cycles to lock after register writes.
     * Same timing requirement applies regardless of pixel format. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Drain until 5 consecutive valid frames confirm AEC has converged. */
    int consec_ok = 0;
    for (int i = 0; i < 30 && consec_ok < 5; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "AEC drain: fb_get timeout i=%d", i);
            consec_ok = 0;
            continue;
        }
        if (fb->len > 0) {
            consec_ok++;
        } else {
            consec_ok = 0;
        }
        esp_camera_fb_return(fb);
    }
    ESP_LOGI(TAG, "AEC stability drain: consec_ok=%d/5", consec_ok);
    if (consec_ok < 5) {
        ESP_LOGE(TAG, "Camera AEC did not stabilize");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    s_camera_ready = true;
    ESP_LOGI(TAG, "Camera ready: OV5640 QVGA YUV422 xclk=%u fb=%d psram_dma=%s",
             (unsigned)CAM_XCLK_HZ,
             CAM_FB_COUNT,
             esp_camera_get_psram_mode() ? "enabled" : "disabled");
    return ESP_OK;
}

void camera_deinit(void)
{
    if (!s_camera_ready) return;
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_deinit: %s", esp_err_to_name(err));
    }
    s_camera_ready = false;
    ESP_LOGI(TAG, "Camera deinitialized");
}

bool camera_is_initialized(void)
{
    return s_camera_ready;
}
