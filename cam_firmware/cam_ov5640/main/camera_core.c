#include "camera_core.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CAM_CORE";

// Pins match original working camera_stream.c (temp_mjpeg commit) exactly.
// UART bridge moved to GPIO1/2 to free GPIO17/18 for camera D6/D5.
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

static bool s_is_initialized = false;

esp_err_t camera_core_init(void)
{
    if (s_is_initialized) return ESP_OK;

    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        // XCLK 20MHz is safe since we have no LVGL/ESP-NOW contention
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG, 
        .frame_size   = FRAMESIZE_QVGA, 
        .jpeg_quality = 40,
        .fb_count     = 2,              // 2 buffers
        .fb_location  = CAMERA_FB_IN_PSRAM, // Safe because bus is empty!
        .grab_mode    = CAMERA_GRAB_LATEST
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed: %s", esp_err_to_name(err));
        return err;
    }

    s_is_initialized = true;
    ESP_LOGI(TAG, "Camera perfectly initialized via PSRAM DMA");
    return ESP_OK;
}

void camera_core_deinit(void)
{
    if (!s_is_initialized) return;
    esp_camera_deinit();
    s_is_initialized = false;
    ESP_LOGI(TAG, "Camera deinitialized");
}
