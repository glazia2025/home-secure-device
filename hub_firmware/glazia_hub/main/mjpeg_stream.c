#include "mjpeg_stream.h"

#include <stdint.h>

#include "camera_stream.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_control_ws.h"

static const char *TAG = "MJPEG";

/* ~7.7 fps — conservative starting point; tune upward after first confirmed
 * working test. Higher rates increase WS send-mutex contention with JSON
 * control messages (door_lock_ack etc.). */
#define MJPEG_INTER_FRAME_MS   130

/* Hard cap on frame size before sending. QVGA JPEG at quality 12 is typically
 * 8–15 KB; frames above this limit are pathological and would hold the shared
 * WS send mutex too long, delaying door_lock_ack. Drop and continue. */
#define MJPEG_MAX_FRAME_BYTES  25600

static volatile bool s_running;
static TaskHandle_t  s_task;

/* ── Transport abstraction ────────────────────────────────────────────────────
 * All frame sends go through this one function. To switch to a dedicated
 * live-feed WebSocket instead of the control WS:
 *   (a) open a second esp_websocket_client handle in mjpeg_stream.c or elsewhere,
 *   (b) replace hub_control_ws_send_bin() below with a call to that handle.
 * The capture loop, inter-frame cadence, size cap, and lifecycle above are
 * untouched — only this function changes.
 *
 * Send timeout is 500 ms (not the 3000 ms used for JSON control messages).
 * This bounds the worst-case mutex hold time so a large frame send cannot
 * delay door_lock_ack by more than 500 ms. Frames are dropped on timeout. */
static esp_err_t mjpeg_transport_send_frame(const uint8_t *data, size_t len)
{
    return hub_control_ws_send_bin(data, len, pdMS_TO_TICKS(500));
}

static void mjpeg_frame_task(void *arg)
{
    (void)arg;

    esp_err_t err = camera_ensure_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "MJPEG session started (inter_frame=%d ms, max_frame=%d B)",
             MJPEG_INTER_FRAME_MS, MJPEG_MAX_FRAME_BYTES);

    while (s_running) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (fb->len == 0) {
            ESP_LOGW(TAG, "Empty frame from camera, skipping");
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(MJPEG_INTER_FRAME_MS));
            continue;
        }

        if (fb->len > MJPEG_MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "Frame too large (%u B > %d B cap), dropping",
                     (unsigned)fb->len, MJPEG_MAX_FRAME_BYTES);
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(MJPEG_INTER_FRAME_MS));
            continue;
        }

        /* fb_return is called before the inter-frame delay on all paths below,
         * so the camera buffer is never held across the sleep. */
        esp_err_t send_err = mjpeg_transport_send_frame(fb->buf, fb->len);
        esp_camera_fb_return(fb);

        if (send_err != ESP_OK) {
            ESP_LOGW(TAG, "Frame send failed (WS busy or disconnected), dropping");
        } else {
            static uint32_t s_frames = 0;
            if ((s_frames++ % 10) == 0) {
                ESP_LOGI(TAG, "Sent JPEG frame over control WS (%d bytes)", (int)fb->len);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MJPEG_INTER_FRAME_MS));
    }

    camera_deinit();
    ESP_LOGI(TAG, "MJPEG session stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

void mjpeg_stream_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "MJPEG already running, ignoring duplicate trigger");
        return;
    }
    s_running = true;
    BaseType_t ret = xTaskCreate(mjpeg_frame_task, "mjpeg_feed",
                                 4096, NULL,
                                 2,   /* below hub_ws(~5), webrtc_loop(4), webrtc_video(3) */
                                 &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate mjpeg_feed failed");
        s_running = false;
        s_task = NULL;
    }
}

void mjpeg_stream_stop(void)
{
    if (!s_running) return;
    s_running = false;
    /* Task sees s_running=false on its next loop iteration, calls camera_deinit(),
     * nulls s_task, and self-deletes. No vTaskDelete() here — avoids deleting
     * the task mid-frame which would leak the in-flight camera buffer. */
    ESP_LOGI(TAG, "MJPEG stop requested");
}
