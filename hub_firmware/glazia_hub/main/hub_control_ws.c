#include "hub_control_ws.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "display.h"
#include "door_lock.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "espnow.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_storage.h"
#include "state.h"
#include "cam_uart.h"

static const char *TAG = "HUB_WS";

static esp_websocket_client_handle_t s_client;
static bool s_started;
static char *s_ws_message_buf;
static size_t s_ws_message_expected;

#define HUB_WS_MAX_TEXT_BYTES 8192

static void log_tls_heap(const char *context)
{
    ESP_LOGI(TAG, "%s: internal_free=%u internal_largest=%u",
             context,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static const char *message_user_name(cJSON *root)
{
    cJSON *user = cJSON_GetObjectItem(root, "user");
    cJSON *owner = cJSON_GetObjectItem(root, "owner");
    cJSON *name = NULL;

    if (cJSON_IsObject(user)) {
        name = cJSON_GetObjectItem(user, "name");
    }
    if (!cJSON_IsString(name) && cJSON_IsObject(owner)) {
        name = cJSON_GetObjectItem(owner, "name");
    }
    if (!cJSON_IsString(name)) {
        name = cJSON_GetObjectItem(root, "userName");
    }

    return cJSON_IsString(name) && name->valuestring ? name->valuestring : NULL;
}

void hub_control_ws_send_json(const char *json_str)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        ESP_LOGW(TAG, "Cannot send JSON; websocket not connected");
        return;
    }
    int len = (int)strlen(json_str);
    int sent = esp_websocket_client_send_text(s_client, json_str, len, pdMS_TO_TICKS(3000));
    if (sent < 0) {
        ESP_LOGW(TAG, "hub_control_ws_send_json failed (len=%d)", len);
    }
}

static void send_door_lock_ack(const char *command_id,
                               const char *status,
                               const char *lock_state,
                               const char *error)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        ESP_LOGW(TAG, "Cannot send ACK; websocket is not connected");
        return;
    }

    char message[256];
    if (error && error[0]) {
        snprintf(message, sizeof(message),
                 "{\"type\":\"door_lock_ack\",\"commandId\":\"%s\",\"status\":\"%s\",\"lockState\":\"%s\",\"error\":\"%s\"}",
                 command_id, status, lock_state, error);
    } else {
        snprintf(message, sizeof(message),
                 "{\"type\":\"door_lock_ack\",\"commandId\":\"%s\",\"status\":\"%s\",\"lockState\":\"%s\"}",
                 command_id, status, lock_state);
    }

    int sent = esp_websocket_client_send_text(s_client, message, strlen(message), pdMS_TO_TICKS(3000));
    if (sent < 0) {
        ESP_LOGW(TAG, "Failed to send door lock ACK id=%s", command_id);
    } else {
        ESP_LOGI(TAG, "Sent door lock ACK id=%s status=%s", command_id, status);
    }
}

static void handle_door_lock_command(cJSON *root)
{
    cJSON *command_id = cJSON_GetObjectItem(root, "commandId");
    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *duration_ms = cJSON_GetObjectItem(root, "durationMs");

    if (!cJSON_IsString(command_id) || !command_id->valuestring ||
        !cJSON_IsString(mode) || !mode->valuestring ||
        !cJSON_IsString(action) || !action->valuestring) {
        ESP_LOGW(TAG, "Door lock command missing required fields");
        return;
    }

    door_lock_command_t command = {0};
    strncpy(command.command_id, command_id->valuestring, sizeof(command.command_id) - 1);
    strncpy(command.mode, mode->valuestring, sizeof(command.mode) - 1);
    strncpy(command.action, action->valuestring, sizeof(command.action) - 1);
    command.duration_ms = cJSON_IsNumber(duration_ms) && duration_ms->valuedouble > 0
                              ? (uint32_t)duration_ms->valuedouble
                              : 0;

    esp_err_t err = door_lock_enqueue(&command);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enqueue door command: %s", esp_err_to_name(err));
        send_door_lock_ack(command.command_id, "failed", "locked", "Hub failed to queue command");
    }
}

static void handle_viewer_ready(void)
{
    ESP_LOGI(TAG, "viewer-ready → cam_uart_webrtc_start");
    cam_uart_webrtc_start(g_wifi_ssid, g_wifi_password, g_turn_user, g_turn_psw);
}

static void handle_viewer_gone(void)
{
    ESP_LOGI(TAG, "viewer-gone → cam_uart_webrtc_stop");
    cam_uart_webrtc_stop();
}

static void handle_answer(cJSON *root)
{
    /* { "type":"answer", "sdp":{"type":"answer","sdp":"<raw_sdp>"}, "hubId":"..." } */
    cJSON *sdp_obj = cJSON_GetObjectItem(root, "sdp");
    if (!cJSON_IsObject(sdp_obj)) {
        ESP_LOGW(TAG, "answer: missing sdp object");
        return;
    }
    cJSON *sdp_str = cJSON_GetObjectItem(sdp_obj, "sdp");
    if (!cJSON_IsString(sdp_str) || !sdp_str->valuestring) {
        ESP_LOGW(TAG, "answer: missing sdp.sdp string");
        return;
    }
    cam_uart_relay_answer(sdp_str->valuestring);
}

static void handle_ice_candidate(cJSON *root)
{
    /* { "type":"ice-candidate", "candidate":{"candidate":"<raw>","sdpMid":"0",...} } */
    cJSON *cand_obj = cJSON_GetObjectItem(root, "candidate");
    if (!cJSON_IsObject(cand_obj)) {
        ESP_LOGW(TAG, "ice-candidate: missing candidate object");
        return;
    }
    cJSON *cand_str = cJSON_GetObjectItem(cand_obj, "candidate");
    if (!cJSON_IsString(cand_str) || !cand_str->valuestring) {
        ESP_LOGW(TAG, "ice-candidate: missing candidate string");
        return;
    }
    ESP_LOGI(TAG, "Relaying ICE candidate to cam_esp: %.80s", cand_str->valuestring);
    cam_uart_relay_ice_to_cam(cand_str->valuestring);
}

static void handle_sensor_delete_command(cJSON *root)
{
    cJSON *mac = cJSON_GetObjectItem(root, "sensorMacAddress");
    if (!cJSON_IsString(mac) || !mac->valuestring || mac->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "sensor_delete_command missing sensorMacAddress");
        return;
    }
    esp_err_t err = espnow_remove_sensor(mac->valuestring);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sensor %s removed via server command", mac->valuestring);
        display_sensor_list();
    } else {
        ESP_LOGW(TAG, "Sensor %s not in peer table (already removed?)", mac->valuestring);
    }
}

static void handle_hub_reset_command(cJSON *root)
{
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action) || strcmp(action->valuestring, "format_and_reset") != 0) {
        ESP_LOGW(TAG, "hub_reset_command: unknown action '%s' — ignoring",
                 cJSON_IsString(action) ? action->valuestring : "null");
        return;
    }
    ESP_LOGW(TAG, "hub_reset_command received — notifying sensors, erasing NVS, restarting");
    espnow_send_reset_to_all_sensors();
    nvs_clear_credentials();
    nvs_prov_clear();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void handle_sensor_toggle_command(cJSON *root)
{
    cJSON *mac = cJSON_GetObjectItem(root, "sensorMacAddress");
    cJSON *en  = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsString(mac) || !mac->valuestring || !cJSON_IsBool(en)) {
        ESP_LOGW(TAG, "sensor_toggle_command: missing fields");
        return;
    }
    bool state = cJSON_IsTrue(en);
    if (espnow_set_sensor_enabled_by_mac(mac->valuestring, state) == ESP_OK)
        ESP_LOGI(TAG, "Sensor %s %s via server", mac->valuestring, state ? "enabled" : "disabled");
    else
        ESP_LOGW(TAG, "sensor_toggle_command: %s not found", mac->valuestring);
}

static void handle_ws_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Invalid websocket JSON");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        if (strcmp(type->valuestring, "ready") == 0) {
            ESP_LOGI(TAG, "Control websocket ready");
            log_tls_heap("WSS ready");
            const char *name = message_user_name(root);
            if (name && name[0] != '\0' && strcmp(name, g_user_name) != 0) {
                strncpy(g_user_name, name, sizeof(g_user_name) - 1);
                g_user_name[sizeof(g_user_name) - 1] = '\0';
                nvs_save_credentials();
                display_user_name(g_user_name);
                ESP_LOGI(TAG, "Username updated from ready event");
            }
            espnow_queue_hub_event("hub_online");
        } else if (strcmp(type->valuestring, "door_lock_command") == 0) {
            handle_door_lock_command(root);
        } else if (strcmp(type->valuestring, "viewer-ready") == 0) {
            handle_viewer_ready();
        } else if (strcmp(type->valuestring, "viewer-gone") == 0) {
            handle_viewer_gone();
        } else if (strcmp(type->valuestring, "answer") == 0) {
            handle_answer(root);
        } else if (strcmp(type->valuestring, "ice-candidate") == 0) {
            handle_ice_candidate(root);
        } else if (strcmp(type->valuestring, "sensor_toggle_command") == 0) {
            handle_sensor_toggle_command(root);
        } else if (strcmp(type->valuestring, "sensor_delete_command") == 0) {
            handle_sensor_delete_command(root);
        } else if (strcmp(type->valuestring, "hub_reset_command") == 0) {
            handle_hub_reset_command(root);
        } else if (strcmp(type->valuestring, "door_lock_ack_received") == 0) {
            ESP_LOGI(TAG, "Server received door lock ACK");
        } else if (strcmp(type->valuestring, "error") == 0) {
            cJSON *error = cJSON_GetObjectItem(root, "error");
            ESP_LOGW(TAG, "Server websocket error: %s",
                     cJSON_IsString(error) ? error->valuestring : "unknown");
        }
    }

    cJSON_Delete(root);
}

static void handle_ws_fragment(const esp_websocket_event_data_t *data)
{
    if (!data || !data->data_ptr || data->data_len <= 0 || !s_ws_message_buf) return;
    if (data->op_code != 0x1 && data->op_code != 0x0) return;

    size_t total = data->payload_len > 0 ? (size_t)data->payload_len : (size_t)data->data_len;
    size_t offset = data->payload_offset >= 0 ? (size_t)data->payload_offset : 0;
    if (offset == 0) {
        s_ws_message_expected = total;
        if (total > HUB_WS_MAX_TEXT_BYTES) {
            ESP_LOGW(TAG, "Dropping oversized websocket text payload (%u bytes)", (unsigned)total);
            s_ws_message_expected = 0;
            return;
        }
    }
    if (s_ws_message_expected == 0 || offset + (size_t)data->data_len > s_ws_message_expected ||
        offset + (size_t)data->data_len > HUB_WS_MAX_TEXT_BYTES) {
        ESP_LOGW(TAG, "Invalid websocket fragment offset=%u len=%d total=%u",
                 (unsigned)offset, data->data_len, (unsigned)s_ws_message_expected);
        s_ws_message_expected = 0;
        return;
    }

    memcpy(s_ws_message_buf + offset, data->data_ptr, (size_t)data->data_len);
    if (offset + (size_t)data->data_len == s_ws_message_expected) {
        s_ws_message_buf[s_ws_message_expected] = '\0';
        handle_ws_text(s_ws_message_buf, (int)s_ws_message_expected);
        s_ws_message_expected = 0;
    }
}

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to hub control websocket");
        log_tls_heap("WSS connected");
        cam_uart_resend_pending_offer();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Hub control websocket disconnected");
        cam_uart_webrtc_stop();
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "Hub control websocket error");
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_ws_fragment(data);
        break;
    default:
        break;
    }
}

esp_err_t hub_control_ws_start(void)
{
    if (s_started) return ESP_OK;

    if (strlen(g_hub_mac) == 0 || strlen(g_hub_secret) == 0) {
        ESP_LOGW(TAG, "Cannot start websocket before hub credentials are ready");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = door_lock_start(send_door_lock_ack);
    if (err != ESP_OK) return err;

    if (!s_ws_message_buf) {
        s_ws_message_buf = heap_caps_malloc(HUB_WS_MAX_TEXT_BYTES + 1, MALLOC_CAP_SPIRAM);
        if (!s_ws_message_buf) {
            ESP_LOGE(TAG, "Failed to allocate websocket assembly buffer in PSRAM");
            return ESP_ERR_NO_MEM;
        }
    }
    s_ws_message_expected = 0;

    static char uri[160];
    static char headers[384];
    snprintf(uri, sizeof(uri), "wss://%s:%d/api/device/hubs/control/ws", SERVER_IP, SERVER_PORT);
    snprintf(headers, sizeof(headers),
             "X-Device-Api-Key: %s\r\n"
             "X-Hub-Mac-Address: %s\r\n"
             "X-Hub-Secret: %s\r\n",
             DEVICE_API_KEY, g_hub_mac, g_hub_secret);

    log_tls_heap("Before WSS start");

    esp_websocket_client_config_t config = {
        .uri               = uri,
        .headers           = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_name         = "hub_ws",
        .task_stack = 4096,
        .buffer_size = 5120,  /* SDP strings are usually 2-4 KB; keep WSS memory below event TLS pressure. */
        .network_timeout_ms = 20000,
        .reconnect_timeout_ms = 5000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 10,
        .keep_alive_enable = true,
    };

    s_client = esp_websocket_client_init(&config);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_register_events failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Hub control websocket started: %s", uri);
    return ESP_OK;
}

void hub_control_ws_stop(void)
{
    if (!s_client) {
        s_started = false;
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

    s_client = NULL;
    s_started = false;
}
