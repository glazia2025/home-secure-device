#include "websocket.h"
#include "state.h"
#include "display.h"
#include "espnow.h"
#include "api_client.h"

#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WS";
static esp_websocket_client_handle_t ws_client = NULL;

// Stores the paired sensor MAC once received
static char s_sensor_mac[18] = {0};

// ── WebSocket event handler ───────────────────────────────────────────────

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {

    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected to server");
        display_show("SENSOR PAIRING", "Scan QR on laptop");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->data_len > 0) {
            char *msg = strndup(data->data_ptr, data->data_len);
            ESP_LOGI(TAG, "WS message: %s", msg);

            // Parse JSON
            cJSON *root = cJSON_Parse(msg);
            if (root) {
                cJSON *event = cJSON_GetObjectItem(root, "event");
                if (event && strcmp(event->valuestring, "SENSOR_PAIRED") == 0) {
                    cJSON *mac = cJSON_GetObjectItem(root, "sensorMacAddress");
                    cJSON *name = cJSON_GetObjectItem(root, "sensorName");

                    if (mac) {
                        strncpy(s_sensor_mac, mac->valuestring, sizeof(s_sensor_mac) - 1);
                        ESP_LOGI(TAG, "Got sensor MAC: %s", s_sensor_mac);

                        char disp[32];
                        snprintf(disp, sizeof(disp), "%s",
                            name ? name->valuestring : s_sensor_mac);
                        display_show("Sensor Found!", disp);

                        // Trigger ESP-NOW pairing
                        espnow_pair_sensor(s_sensor_mac);
                    }
                }
                cJSON_Delete(root);
            }
            free(msg);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void websocket_start(void)
{
    ESP_LOGI(TAG, "Connecting WebSocket to %s", WS_URI);

    // Build URI with auth headers as query params
    // (ESP WebSocket client doesn't support custom headers easily,
    //  so we pass mac and secret as query params)
    char uri[256];
    snprintf(uri, sizeof(uri),
        "ws://" SERVER_IP ":%d/api/device/hubs/ws?mac=%s&secret=%s",
        SERVER_PORT, g_hub_mac, g_hub_secret);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
    };

    ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
        ws_event_handler, NULL);
    esp_websocket_client_start(ws_client);
}

void websocket_stop(void)
{
    if (ws_client) {
        // Send acknowledge message before closing
        char ack[128];
        snprintf(ack, sizeof(ack),
            "{\"event\":\"SENSOR_ACKNOWLEDGED\",\"sensorMacAddress\":\"%s\"}",
            s_sensor_mac);
        esp_websocket_client_send_text(ws_client, ack, strlen(ack), pdMS_TO_TICKS(2000));

        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
        ESP_LOGI(TAG, "WebSocket closed");
    }
}
