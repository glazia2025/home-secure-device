#include "camera_media_ws.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "state.h"

static const char *TAG = "CAMERA_WS";

static esp_websocket_client_handle_t s_client;
static bool s_connected;
static bool s_started;
static uint32_t s_sent_count;
static char s_stream_session_id[80];

static void reset_media_state(void)
{
    s_client = NULL;
    s_connected = false;
    s_started = false;
    s_sent_count = 0;
    memset(s_stream_session_id, 0, sizeof(s_stream_session_id));
}

static void camera_ws_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Camera media websocket connected stream=%.8s", s_stream_session_id);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Camera media websocket disconnected stream=%.8s", s_stream_session_id);
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_connected = false;
        ESP_LOGW(TAG, "Camera media websocket error stream=%.8s", s_stream_session_id);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data && data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {
            ESP_LOGI(TAG, "Camera media websocket text: %.*s", data->data_len, data->data_ptr);
        }
        break;
    default:
        break;
    }
}

esp_err_t camera_media_ws_connect(const char *stream_session_id)
{
    if (s_started && s_client) {
        if (esp_websocket_client_is_connected(s_client)) {
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (strlen(g_hub_mac) == 0 || strlen(g_hub_secret) == 0) {
        ESP_LOGW(TAG, "Cannot start camera media websocket before hub credentials are ready");
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_stream_session_id, 0, sizeof(s_stream_session_id));
    if (stream_session_id && stream_session_id[0]) {
        strncpy(s_stream_session_id, stream_session_id, sizeof(s_stream_session_id) - 1);
    }

    static char uri[192];
    static char headers[448];
    snprintf(uri, sizeof(uri), "ws://%s:%d/api/device/hubs/camera/ws", SERVER_IP, SERVER_PORT);
    snprintf(headers, sizeof(headers),
             "X-Device-Api-Key: %s\r\n"
             "X-Hub-Mac-Address: %s\r\n"
             "X-Hub-Secret: %s\r\n"
             "X-Camera-Stream-Session: %s\r\n",
             DEVICE_API_KEY, g_hub_mac, g_hub_secret, s_stream_session_id);

    esp_websocket_client_config_t config = {
        .uri = uri,
        .headers = headers,
        .task_name = "camera_ws",
        /* 6144 matches the camera_stream task stack; the WS client task needs
         * room for lwip send path + the large binary frame send call. 4096
         * was marginal when esp_websocket_client_send_bin is called on a
         * ~3-8 KB JPEG buffer – it copies through the internal send buffer. */
        .task_stack = 6144,
        /* 1024 bytes: enough for the HTTP upgrade handshake response from the
         * server (which includes several headers). The original 512 was
         * sometimes causing the upgrade to be read in two chunks, confusing
         * the WS client state machine on slow Wi-Fi paths. */
        .buffer_size = 10240,
        .network_timeout_ms = 10000,
        /* Disable reconnect: the media WS lifecycle is managed by camera_task.
         * If the server closes it, camera_task should notice via
         * camera_media_ws_is_connected() returning false and stop streaming,
         * not silently reconnect mid-stream. */
        .reconnect_timeout_ms = 0,
        .disable_auto_reconnect = true,
        .ping_interval_sec = 0,
        .keep_alive_enable = false,
    };

    ESP_LOGI(TAG,
             "Starting camera media websocket: %s stream=%.8s internal_free=%u internal_largest=%u",
             uri,
             s_stream_session_id,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_client = esp_websocket_client_init(&config);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        reset_media_state();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, camera_ws_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_register_events failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        reset_media_state();
        return err;
    }

    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        reset_media_state();
        return err;
    }

    s_started = true;
    s_sent_count = 0;
    ESP_LOGI(TAG, "Camera media websocket started: %s stream=%.8s", uri, s_stream_session_id);
    return ESP_OK;
}

esp_err_t camera_media_ws_send_frame(const uint8_t *jpg, size_t jpg_len)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 10 s covers one full TCP RTO cycle (~1–3 s first retransmit) plus WiFi
     * recovery time after a DELBA event. portMAX_DELAY is NOT used: the WS
     * client task calls abort_connection (which needs the same WS mutex) when
     * the server closes the socket, so portMAX_DELAY would deadlock. */
    int sent = esp_websocket_client_send_bin(s_client, (const char *)jpg, jpg_len, pdMS_TO_TICKS(10000));

    // Fix: catch 0 (timeout) as an explicit failure
    if (sent <= 0) {
        ESP_LOGW(TAG, "Camera media frame send failed stream=%.8s bytes=%u", s_stream_session_id, (unsigned)jpg_len);
        return ESP_FAIL;
    }

    s_sent_count++;
    if (s_sent_count == 1 || (s_sent_count % 30) == 0) {
        ESP_LOGI(TAG, "Camera media frame sent stream=%.8s bytes=%u count=%" PRIu32,
                 s_stream_session_id, (unsigned)jpg_len, s_sent_count);
    }
    return ESP_OK;
}
void camera_media_ws_stop(void)
{
    if (!s_client) {
        reset_media_state();
        return;
    }

    esp_err_t err = esp_websocket_client_stop(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_websocket_client_stop failed: %s", esp_err_to_name(err));
    }

    err = esp_websocket_client_destroy(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_websocket_client_destroy failed: %s", esp_err_to_name(err));
    }

    reset_media_state();
}

bool camera_media_ws_is_connected(void)
{
    return s_client && s_connected && esp_websocket_client_is_connected(s_client);
}
