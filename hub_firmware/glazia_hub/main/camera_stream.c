#include "camera_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"
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

#define JPEG_QUALITY       4
#define FRAME_DELAY_MS     33
#define SEED_EXPOSURE      1600
#define SEED_GAIN          16
#define SEED_FRAMES        15
#define SEED_FRAME_DELAY_MS 40
#define MAX_FRAME_BYTES    (300 * 1024)
#define DISPLAY_WAIT_MS    10000
#define INIT_RETRY_DELAY_MS 1000

static const char *TAG = "CAMERA";

static TaskHandle_t s_task;
static volatile bool s_should_run;
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

static esp_err_t camera_init_once(void)
{
    if (s_camera_ready) return ESP_OK;

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "Camera init memory: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

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
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,
        .jpeg_quality = JPEG_QUALITY,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
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
    s->set_lenc(s,          1);
    s->set_denoise(s,       1);
    s->set_hmirror(s,       1);
    s->set_vflip(s,         0);
    s->set_quality(s,       JPEG_QUALITY);

    aec_set_center_weighted(s);

    s->set_exposure_ctrl(s, 0);
    s->set_gain_ctrl(s,     0);
    s->set_aec_value(s,     SEED_EXPOSURE);
    s->set_agc_gain(s,      SEED_GAIN);

    ESP_LOGI(TAG, "Seeding exposure: %d lines, gain %dx, flushing %d frames",
             SEED_EXPOSURE, SEED_GAIN, SEED_FRAMES);
    int seed_misses = 0;
    for (int i = 0; i < SEED_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        } else {
            seed_misses++;
            ESP_LOGW(TAG, "Exposure seed frame %d/%d timed out", i + 1, SEED_FRAMES);
        }
        vTaskDelay(pdMS_TO_TICKS(SEED_FRAME_DELAY_MS));
    }
    if (seed_misses > 0) {
        ESP_LOGW(TAG, "Exposure seed completed with %d missed frame(s)", seed_misses);
    }

    s->set_gain_ctrl(s,     1);
    s->set_gainceiling(s,   GAINCEILING_64X);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s,          1);
    s->set_ae_level(s,      2);

    s_camera_ready = true;
    ESP_LOGI(TAG, "Camera ready: OV5640 VGA JPEG q=%d", JPEG_QUALITY);
    return ESP_OK;
}

static esp_err_t upload_frame(const uint8_t *jpg, size_t jpg_len)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/api/device/hubs/camera/frame", SERVER_BASE);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);
    esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    esp_http_client_set_header(client, "X-Hub-Secret", g_hub_secret);
    esp_http_client_set_post_field(client, (const char *)jpg, jpg_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Frame upload transport failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Frame upload rejected: HTTP %d bytes=%u", status, (unsigned)jpg_len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void camera_task(void *arg)
{
    (void)arg;

    bool display_ready = display_wait_ready(DISPLAY_WAIT_MS);
    ESP_LOGI(TAG, "Display settled before camera init: %s", display_ready ? "ready" : "not ready/failed");

    esp_err_t err = camera_init_once();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Camera init failed once, retrying after cleanup: %s", esp_err_to_name(err));
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_DELAY_MS));
        err = camera_init_once();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera task stopping after init failure");
            s_task = NULL;
            s_should_run = false;
            vTaskDelete(NULL);
        }
    }

    uint32_t seq = 0;
    while (s_should_run) {
        if (strlen(g_hub_secret) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Failed to capture frame");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            ESP_LOGW(TAG, "Dropping non-JPEG frame seq=%" PRIu32, seq);
        } else if (fb->len > MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "Dropping oversized frame seq=%" PRIu32 " bytes=%u", seq, (unsigned)fb->len);
        } else {
            err = upload_frame(fb->buf, fb->len);
            if (err == ESP_OK && (seq % 30) == 0) {
                ESP_LOGI(TAG, "Uploaded frame seq=%" PRIu32 " %ux%u bytes=%u",
                         seq, fb->width, fb->height, (unsigned)fb->len);
            }
        }

        esp_camera_fb_return(fb);
        seq++;
        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }

    ESP_LOGI(TAG, "Camera stream task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_stream_start(void)
{
    if (s_task) {
        s_should_run = true;
        return ESP_OK;
    }

    if (strlen(g_hub_secret) == 0) {
        ESP_LOGW(TAG, "Cannot start camera stream before hub secret is ready");
        return ESP_ERR_INVALID_STATE;
    }

    s_should_run = true;
    if (xTaskCreate(camera_task, "camera_stream", 8192, NULL, 4, &s_task) != pdPASS) {
        s_should_run = false;
        ESP_LOGE(TAG, "Failed to create camera stream task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void camera_stream_stop(void)
{
    s_should_run = false;
}
