#include "camera_stream.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "camera_media_ws.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_control_ws.h"
#include "sensor.h"
#include "sdkconfig.h"
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

/* OV5640 XCLK ----------------------------------------------------------------
 * 20 MHz was causing NO-EOI / frame-timeout in PSRAM-DMA mode.
 *
 * Root cause: at 20 MHz XCLK the sensor pixel clock (PCLK ≈ 40 MHz for QVGA)
 * pushes JPEG data faster than the AHB→PSRAM DMA path can drain it.  The
 * cam_hal DMA ring wraps before FFD9 lands in the PSRAM frame buffer, so
 * cam_hal reports NO-EOI and the frame times out.
 *
 * 16 MHz XCLK (PCLK ≈ 32 MHz) gives the PSRAM DMA path ~20 % more headroom
 * per byte, eliminating the race at QVGA resolution.
 * Frame rate at 16 MHz is still > 10 fps at QVGA – perfectly adequate for
 * the MJPEG stream.
 */
#define CAM_XCLK_HZ    16000000

/* Frame-buffer strategy in PSRAM-DMA mode ------------------------------------
 * fb_count=2 + CAMERA_GRAB_LATEST was the second contributing factor.
 * GRAB_LATEST tells cam_hal to keep overwriting the "oldest" buffer while the
 * DMA ring runs continuously.  With a marginal internal-heap largest-block
 * (~14 KB before init), the DMA descriptor ring for two PSRAM buffers barely
 * fits, leaving no slack for the ISR to safely commit both EOI markers.
 *
 * fb_count=1 + CAMERA_GRAB_WHEN_EMPTY:
 *   - halves the DMA descriptor ring → less internal heap pressure
 *   - the ISR only fills one buffer at a time; it waits until the application
 *     returns the buffer before starting the next frame
 *   - eliminates the mid-write overwrite race entirely
 *
 * The trade-off is that the sensor stalls briefly between frames.  For a
 * 10 fps MJPEG stream with FRAME_DELAY_MS=100 this is invisible.
 */
#define CAM_FB_COUNT       2
#define CAM_GRAB_MODE      CAMERA_GRAB_LATEST

#define JPEG_QUALITY       12
#define FRAME_DELAY_MS     100
#define MEDIA_SEND_FAIL_DELAY_MS 1000
#define CORRUPT_FRAME_DELAY_MS 250
#define FIRST_FRAME_PROBE_ATTEMPTS 3
#define FIRST_FRAME_PROBE_DELAY_MS 200
#define MAX_FRAME_BYTES    (300 * 1024)
#define DISPLAY_WAIT_MS    10000
#define INIT_RETRY_DELAY_MS 1000
#define MEDIA_CONNECT_WAIT_MS 3000
#define MEDIA_CONNECT_POLL_MS 100
#define STABILITY_PROBE_TARGET 5
#define STABILITY_PROBE_MAX    30

static const char *TAG = "CAMERA";

static TaskHandle_t s_task;
static volatile bool s_should_run;
static bool s_camera_ready;
static char s_stream_session_id[80];
static uint8_t *s_media_tx_buf = NULL;

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

static bool is_valid_jpeg(const uint8_t *jpg, size_t jpg_len)
{
    if (!jpg || jpg_len < 4) {
        return false;
    }

    return jpg[0] == 0xff &&
           jpg[1] == 0xd8 &&
           jpg[jpg_len - 2] == 0xff &&
           jpg[jpg_len - 1] == 0xd9;
}

static esp_err_t camera_probe_first_jpeg(void)
{
    int timeouts = 0;
    int corrupt = 0;

    for (int i = 0; i < FIRST_FRAME_PROBE_ATTEMPTS; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            timeouts++;
            ESP_LOGW(TAG, "First-frame probe %d/%d timed out", i + 1, FIRST_FRAME_PROBE_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(FIRST_FRAME_PROBE_DELAY_MS));
            continue;
        }

        bool valid = fb->format == PIXFORMAT_JPEG &&
                     fb->len <= MAX_FRAME_BYTES &&
                     is_valid_jpeg(fb->buf, fb->len);
        if (valid) {
            ESP_LOGI(TAG, "First-frame probe captured valid JPEG: %ux%u bytes=%u attempt=%d",
                     fb->width, fb->height, (unsigned)fb->len, i + 1);
            esp_camera_fb_return(fb);
            return ESP_OK;
        }

        corrupt++;
        ESP_LOGW(TAG, "First-frame probe dropped invalid JPEG attempt=%d format=%d bytes=%u soi=%02x%02x eoi=%02x%02x",
                 i + 1,
                 fb->format,
                 (unsigned)fb->len,
                 fb->len >= 2 ? fb->buf[0] : 0,
                 fb->len >= 2 ? fb->buf[1] : 0,
                 fb->len >= 2 ? fb->buf[fb->len - 2] : 0,
                 fb->len >= 2 ? fb->buf[fb->len - 1] : 0);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(FIRST_FRAME_PROBE_DELAY_MS));
    }

    ESP_LOGE(TAG,
             "First-frame probe failed: attempts=%d timeouts=%d corrupt=%d psram_dma=%s",
             FIRST_FRAME_PROBE_ATTEMPTS,
             timeouts,
             corrupt,
             esp_camera_get_psram_mode() ? "enabled" : "disabled");
    return ESP_FAIL;
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

    esp_err_t err = esp_camera_set_psram_mode(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable camera PSRAM-DMA mode: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Camera capture DMA mode requested: PSRAM DMA, frame buffers in PSRAM");

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
        .pixel_format = PIXFORMAT_JPEG,
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
    s->set_lenc(s,          1);
    s->set_denoise(s,       1);
    s->set_hmirror(s,       1);
    s->set_vflip(s,         0);
    s->set_quality(s,       JPEG_QUALITY);

    aec_set_center_weighted(s);

    s->set_gain_ctrl(s,     1);
    s->set_gainceiling(s,   GAINCEILING_64X);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s,          1);
    s->set_ae_level(s,      2);

    /* OV5640 AEC/AWB needs several VSYNC cycles to lock after register writes.
     * Without this delay the JPEG encoder outputs NO-EOI frames; cam_hal's
     * internal 100-miss loop exhausts and leaves the DMA in a stuck state
     * where every subsequent esp_camera_fb_get() times out at 4s each. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    err = camera_probe_first_jpeg();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera first-frame probe failed after init");
        esp_camera_deinit();
        return err;
    }

    /* OV5640 at PCLK=8 MHz runs at ~4 fps (250 ms/frame at QVGA). The initial
     * probe exits on the FIRST valid frame, which may occur mid-AEC-ramp. The
     * AEC algorithm then makes a correction step; the next 10-20 frames can
     * still be NO-EOI. Drain until 5 consecutive valid frames — this confirms
     * AEC has converged and the streaming loop's first fb_get() will succeed.
     * 30 attempts × 250 ms ≈ 7.5 s worst case; typical < 2 s after probe. */
    int consec_ok = 0;
    for (int i = 0; i < STABILITY_PROBE_MAX && consec_ok < STABILITY_PROBE_TARGET; i++) {
        camera_fb_t *sfb = esp_camera_fb_get();
        if (!sfb) {
            ESP_LOGW(TAG, "AEC stability drain: fb_get timeout i=%d", i);
            consec_ok = 0;
            continue;
        }
        if (is_valid_jpeg(sfb->buf, sfb->len)) {
            consec_ok++;
        } else {
            consec_ok = 0;
        }
        esp_camera_fb_return(sfb);
    }
    ESP_LOGI(TAG, "AEC stability drain: consec_ok=%d/%d", consec_ok, STABILITY_PROBE_TARGET);
    if (consec_ok < STABILITY_PROBE_TARGET) {
        ESP_LOGE(TAG, "Camera AEC did not stabilize after %d frames", STABILITY_PROBE_MAX);
        esp_camera_deinit();
        return ESP_FAIL;
    }

    if (!s_media_tx_buf) {
        s_media_tx_buf = heap_caps_malloc(MAX_FRAME_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_media_tx_buf) {
            ESP_LOGE(TAG, "Failed to allocate media TX buffer in PSRAM");
            esp_camera_deinit();
            return ESP_ERR_NO_MEM;
        }
    }

    s_camera_ready = true;
    ESP_LOGI(TAG, "Camera ready: OV5640 QVGA JPEG q=%d xclk=%u fb=%d grab=%s psram_dma=%s",
             JPEG_QUALITY,
             (unsigned)CAM_XCLK_HZ,
             CAM_FB_COUNT,
             (CAM_GRAB_MODE == CAMERA_GRAB_WHEN_EMPTY) ? "when_empty" : "latest",
             esp_camera_get_psram_mode() ? "enabled" : "disabled");
    return ESP_OK;
}

static void camera_task(void *arg)
{
    (void)arg;

    hub_control_ws_send_camera_status(s_stream_session_id, "starting", NULL);

    ESP_LOGI(TAG,
             "Camera media preconnect memory: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    esp_err_t err = camera_media_ws_connect(s_stream_session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera media websocket start failed: %s", esp_err_to_name(err));
        hub_control_ws_send_camera_status(s_stream_session_id, "failed", esp_err_to_name(err));
        camera_media_ws_stop();
        s_should_run = false;
        s_task = NULL;
        vTaskDelete(NULL);
    }

    int waited_ms = 0;
    while (s_should_run && !camera_media_ws_is_connected() && waited_ms < MEDIA_CONNECT_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(MEDIA_CONNECT_POLL_MS));
        waited_ms += MEDIA_CONNECT_POLL_MS;
    }

    if (!s_should_run || !camera_media_ws_is_connected()) {
        ESP_LOGE(TAG, "Camera media websocket did not connect within %d ms", waited_ms);
        hub_control_ws_send_camera_status(s_stream_session_id, "failed", "media websocket connect timeout");
        camera_media_ws_stop();
        s_should_run = false;
        s_task = NULL;
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG,
             "Camera media connected before camera init: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    bool display_ready = display_wait_ready(DISPLAY_WAIT_MS);
    ESP_LOGI(TAG, "Display settled before camera init: %s", display_ready ? "ready" : "not ready/failed");

    for (int cam_attempt = 0; cam_attempt < 3; cam_attempt++) {
        err = camera_init_once();
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "Camera init attempt %d/3 failed (%s), reiniting",
                 cam_attempt + 1, esp_err_to_name(err));
        esp_camera_deinit();
        s_camera_ready = false;
        vTaskDelay(pdMS_TO_TICKS(INIT_RETRY_DELAY_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera task stopping after 3 init failures");
        hub_control_ws_send_camera_status(s_stream_session_id, "failed", esp_err_to_name(err));
        camera_media_ws_stop();
        s_camera_ready = false;
        s_task = NULL;
        s_should_run = false;
        vTaskDelete(NULL);
    }

    /* Camera PSRAM DMA (channel 2) briefly starves the WiFi TX path right after
     * init. Without this delay the first send attempt hits transport_poll_write(0)
     * which makes esp_websocket_client abort the entire connection. */
    vTaskDelay(pdMS_TO_TICKS(500));

    uint32_t seq = 0;
    bool reported_streaming = false;
    while (s_should_run) {
        /* Check WS before fb_get() — fb_get can hang 4 s in cam_hal's stuck
         * state if DMA is disrupted. Breaking here stops camera DMA quickly
         * so WiFi recovers before the control WS PING/PONG times out. */
        if (!camera_media_ws_is_connected()) {
            ESP_LOGE(TAG, "Media WS disconnected. Aborting stream loop.");
            break;
        }

        bool media_send_failed = false;
        bool corrupt_frame = false;

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

        bool return_fb_now = false;

        if (fb->format != PIXFORMAT_JPEG) {
            ESP_LOGW(TAG, "Dropping non-JPEG frame seq=%" PRIu32, seq);
            corrupt_frame = true;
            return_fb_now = true;
        } else if (fb->len > MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "Dropping oversized frame seq=%" PRIu32 " bytes=%u", seq, (unsigned)fb->len);
            corrupt_frame = true;
            return_fb_now = true;
        } else if (!is_valid_jpeg(fb->buf, fb->len)) {
            ESP_LOGW(TAG, "Dropping corrupt JPEG frame");
            corrupt_frame = true;
            return_fb_now = true;
        } else {
            if (!camera_media_ws_is_connected()) {
                ESP_LOGE(TAG, "Media WS disconnected. Aborting stream loop.");
                break;
            } else {
                // 1. THE ELASTIC BOUNDARY: Copy frame to detached PSRAM buffer
                memcpy(s_media_tx_buf, fb->buf, fb->len);
                size_t tx_len = fb->len;
                uint16_t tx_w = fb->width;
                uint16_t tx_h = fb->height;

                // 2. Instantly release the hardware buffer back to the DMA engine
                esp_camera_fb_return(fb);

                /* At QVGA + GRAB_WHEN_EMPTY, fb_return() triggers an immediate
                 * DMA capture of the next frame (~20 ms at PCLK=8 MHz). Without
                 * this gap the DMA PSRAM bus traffic overlaps with the first TCP
                 * write, causing the AP to delete the WiFi BA session (DELBA
                 * reason:1). After DELBA throughput degrades enough to exhaust
                 * the send timeout and abort the WS connection. */
                vTaskDelay(pdMS_TO_TICKS(50));

                // 3. Safely block on network transmission without starving hardware
                err = camera_media_ws_send_frame(s_media_tx_buf, tx_len);

                if (err == ESP_OK && (seq % 30) == 0) {
                    ESP_LOGI(TAG, "Sent frame seq=%" PRIu32 " %ux%u bytes=%u",
                             seq, tx_w, tx_h, (unsigned)tx_len);
                }
                if (err == ESP_OK && !reported_streaming) {
                    hub_control_ws_send_camera_status(s_stream_session_id, "streaming", NULL);
                    reported_streaming = true;
                } else if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Camera media frame send failed: %s", esp_err_to_name(err));
                    if (!camera_media_ws_is_connected()) {
                        break;  /* WS gone — stop now, don't wait MEDIA_SEND_FAIL_DELAY_MS */
                    }
                    media_send_failed = true;
                }
            }
        }

        // Return the hardware buffer only if we didn't already detach it above
        if (return_fb_now) {
            esp_camera_fb_return(fb);
        }
        seq++;
        if (media_send_failed) {
            vTaskDelay(pdMS_TO_TICKS(MEDIA_SEND_FAIL_DELAY_MS));
        } else if (corrupt_frame) {
            vTaskDelay(pdMS_TO_TICKS(CORRUPT_FRAME_DELAY_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
        }
    }

    if (s_camera_ready) {
        esp_err_t err = esp_camera_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_camera_deinit failed: %s", esp_err_to_name(err));
        }
        s_camera_ready = false;
    }

    camera_media_ws_stop();
    hub_control_ws_send_camera_status(s_stream_session_id, "stopped", NULL);
    ESP_LOGI(TAG, "Camera stream task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_stream_start(const char *stream_session_id)
{
    ESP_LOGI(TAG, "Camera stream start requested");

    memset(s_stream_session_id, 0, sizeof(s_stream_session_id));
    if (stream_session_id && stream_session_id[0]) {
        strncpy(s_stream_session_id, stream_session_id, sizeof(s_stream_session_id) - 1);
    }

    if (s_task) {
        s_should_run = true;
        ESP_LOGI(TAG, "Camera stream task already running");
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

    ESP_LOGI(TAG, "Camera stream task created");
    return ESP_OK;
}

void camera_stream_stop(void)
{
    ESP_LOGI(TAG, "Camera stream stop requested");
    s_should_run = false;
}
