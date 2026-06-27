#include "espnow.h"
#include "state.h"
#include "display.h"
#include "api_client.h"
#include "nvs_storage.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "ESPNOW";

// ── Packet types ──────────────────────────────────────────────────────────
#define PKT_HELLO   0x01
#define PKT_ACK     0x02
#define PKT_EVENT   0x03
#define PKT_COMMIT  0x04
#define PKT_RESET   0x05

typedef struct {
    uint8_t type;
    char    payload[128];
} __attribute__((packed)) espnow_packet_t;

// ── ESP-NOW worker queues ──────────────────────────────────────────────────
#define EVENT_QUEUE_DEPTH   40
#define COMMIT_QUEUE_DEPTH  4
#define COMMIT_WORKER_STACK 3072

typedef struct {
    char    mac_str[18];
    char    payload[128];
    bool    is_confirm;
    bool    is_hub_event;
    int8_t  rssi;
    uint8_t retry_count;
} event_item_t;

typedef struct {
    uint8_t mac[6];
    char sensor_mac[18];
    char nonce[17];
} commit_item_t;

static QueueHandle_t s_event_queue = NULL;
static QueueHandle_t s_commit_queue = NULL;
static TaskHandle_t  s_event_task_handle = NULL;
static TaskHandle_t  s_commit_task_handle = NULL;
static bool          s_espnow_ready = false;

static void log_internal_heap(const char *context)
{
    ESP_LOGW(TAG, "%s: internal_free=%u internal_largest=%u",
             context,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void event_forward_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Event forward task started");
    event_item_t item;
    while (1) {
        if (xQueueReceive(s_event_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.is_hub_event) {
                ESP_LOGI(TAG, "Forwarding hub event '%s' to API", item.payload);
                api_send_hub_event(item.payload);
                continue;
            }

            if (item.is_confirm) {
                ESP_LOGI(TAG, "Confirming sensor %s with server", item.mac_str);
                api_confirm_sensor(item.mac_str);
                continue;
            }

            ESP_LOGI(TAG, "Forwarding queued event from %s to API", item.mac_str);

            const char *event_type;
            const char *severity;
            char payload_json[128];

            if (strcmp(item.payload, "door_open") == 0) {
                event_type = "door_opened";
                severity   = "critical";
                snprintf(payload_json, sizeof(payload_json),
                         "{\"module\":\"magnetic_reed\",\"reedState\":\"open\","
                         "\"batteryPercent\":100,\"rssi\":%d}", (int)item.rssi);
            } else if (strcmp(item.payload, "door_close") == 0) {
                event_type = "door_closed";
                severity   = "info";
                snprintf(payload_json, sizeof(payload_json),
                         "{\"module\":\"magnetic_reed\",\"reedState\":\"closed\","
                         "\"batteryPercent\":100,\"rssi\":%d}", (int)item.rssi);
            } else if (strcmp(item.payload, "vibration") == 0) {
                event_type = "shock_detected";
                severity   = "critical";
                snprintf(payload_json, sizeof(payload_json),
                         "{\"module\":\"vibration\",\"shockFound\":true,"
                         "\"batteryPercent\":100,\"rssi\":%d}", (int)item.rssi);
            } else {
                event_type = "sensor_data";
                severity   = "info";
                snprintf(payload_json, sizeof(payload_json),
                         "{\"raw\":\"%.110s\"}", item.payload);
            }

            bool ok = api_send_event(item.mac_str, event_type, severity, payload_json);
            if (ok) {
                ESP_LOGI(TAG, "Event forward result for %s: accepted", item.mac_str);
            } else if (item.retry_count < 3) {
                item.retry_count++;
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (!s_event_queue || xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Event retry queue full for %s — dropping", item.mac_str);
                } else {
                    ESP_LOGI(TAG, "Event re-queued (retry %d/3) for %s", item.retry_count, item.mac_str);
                }
            } else {
                ESP_LOGW(TAG, "Event max retries reached for %s — dropping", item.mac_str);
            }
        }
    }
}

static void send_commit_retries(const commit_item_t *item)
{
    espnow_packet_t commit = { .type = PKT_COMMIT };
    snprintf(commit.payload, sizeof(commit.payload), "H:%s;S:%s;N:%s",
             g_hub_mac, item->sensor_mac, item->nonce);

    for (int i = 0; i < 2; i++) {
        esp_err_t err = esp_now_send(item->mac, (uint8_t *)&commit, sizeof(commit));
        if (i == 0 || err != ESP_OK) {
            ESP_LOGI(TAG, "COMMIT to %s attempt %d/2: %s",
                     item->sensor_mac, i + 1,
                     err == ESP_OK ? "sent" : esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void commit_retry_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "COMMIT retry worker task started");
    commit_item_t item;
    while (1) {
        if (xQueueReceive(s_commit_queue, &item, portMAX_DELAY) == pdTRUE) {
            send_commit_retries(&item);
        }
    }
}

// ── Multi-sensor table ────────────────────────────────────────────────────
// sdkconfig: CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM=10
#define MAX_SENSORS 10

typedef struct {
    uint8_t mac[6];
    uint8_t lmk[16];   // LMK = provision_key bytes
    bool    paired;    // true once ACK received
    bool    enabled;
    bool    is_reconnect;  // true = loaded from NVS; skip server confirm on ACK
    char    name[32];
    char    zone[32];
    char    nonce[17];
    bool    notify_on_ack;
} sensor_entry_t;

static sensor_entry_t s_sensors[MAX_SENSORS];
static int            s_sensor_count = 0;

// ── Helpers ───────────────────────────────────────────────────────────────

static void send_pkt_reset(const uint8_t *mac);  // defined below public API

static void mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes)
{
    sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &mac_bytes[0], &mac_bytes[1], &mac_bytes[2],
        &mac_bytes[3], &mac_bytes[4], &mac_bytes[5]);
}

static void mac_bytes_to_str(const uint8_t *mac, char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void hex_to_bytes(const char *hex, uint8_t *out, int out_len)
{
    for (int i = 0; i < out_len; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        out[i] = (uint8_t)strtol(b, NULL, 16);
    }
}

static int sensor_index_from_entry(sensor_entry_t *entry)
{
    if (!entry) return -1;
    return (int)(entry - s_sensors);
}

static sensor_entry_t *find_sensor_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (memcmp(s_sensors[i].mac, mac, 6) == 0) {
            return &s_sensors[i];
        }
    }
    return NULL;
}

static void remove_sensor_entry(sensor_entry_t *entry)
{
    int index = sensor_index_from_entry(entry);
    if (index < 0 || index >= s_sensor_count) return;

    esp_now_del_peer(entry->mac);
    for (int i = index; i < s_sensor_count - 1; i++) {
        s_sensors[i] = s_sensors[i + 1];
    }
    memset(&s_sensors[s_sensor_count - 1], 0, sizeof(s_sensors[0]));
    s_sensor_count--;
}

static void make_nonce(char out[17])
{
    uint32_t hi = esp_random();
    uint32_t lo = esp_random();
    snprintf(out, 17, "%08" PRIX32 "%08" PRIX32, hi, lo);
}

static bool parse_pair_payload(const char *payload,
                               char *hub_mac, size_t hub_mac_len,
                               char *sensor_mac, size_t sensor_mac_len,
                               char *nonce, size_t nonce_len)
{
    char parsed_hub[18] = {0};
    char parsed_sensor[18] = {0};
    char parsed_nonce[17] = {0};
    int matched = sscanf(payload, "H:%17[^;];S:%17[^;];N:%16s",
                         parsed_hub, parsed_sensor, parsed_nonce);
    if (matched != 3 || strlen(parsed_hub) != 17 ||
        strlen(parsed_sensor) != 17 || strlen(parsed_nonce) != 16) {
        return false;
    }
    if (hub_mac && hub_mac_len > 0) {
        strncpy(hub_mac, parsed_hub, hub_mac_len - 1);
        hub_mac[hub_mac_len - 1] = '\0';
    }
    if (sensor_mac && sensor_mac_len > 0) {
        strncpy(sensor_mac, parsed_sensor, sensor_mac_len - 1);
        sensor_mac[sensor_mac_len - 1] = '\0';
    }
    if (nonce && nonce_len > 0) {
        strncpy(nonce, parsed_nonce, nonce_len - 1);
        nonce[nonce_len - 1] = '\0';
    }
    return true;
}

static void commit_sensor_table(void)
{
    char    saved_macs[MAX_SENSORS][18];
    uint8_t saved_keys[MAX_SENSORS][16];
    char    saved_names[MAX_SENSORS][32];
    char    saved_zones[MAX_SENSORS][32];
    int     saved_count = 0;

    for (int i = 0; i < s_sensor_count; i++) {
        mac_bytes_to_str(s_sensors[i].mac, saved_macs[saved_count]);
        memcpy(saved_keys[saved_count], s_sensors[i].lmk, 16);
        strncpy(saved_names[saved_count], s_sensors[i].name, sizeof(saved_names[0]) - 1);
        strncpy(saved_zones[saved_count], s_sensors[i].zone, sizeof(saved_zones[0]) - 1);
        saved_names[saved_count][sizeof(saved_names[0]) - 1] = '\0';
        saved_zones[saved_count][sizeof(saved_zones[0]) - 1] = '\0';
        saved_count++;
    }

    nvs_save_sensors(saved_macs, saved_keys, saved_names, saved_zones, saved_count);
}

static bool queue_commit_retry(sensor_entry_t *entry, const char *sensor_mac)
{
    if (!s_commit_queue) {
        ESP_LOGE(TAG, "Cannot queue COMMIT for %s: COMMIT queue missing", sensor_mac);
        log_internal_heap("COMMIT queue missing");
        return false;
    }

    commit_item_t item = {0};
    memcpy(item.mac, entry->mac, 6);
    strncpy(item.sensor_mac, sensor_mac, sizeof(item.sensor_mac) - 1);
    strncpy(item.nonce, entry->nonce, sizeof(item.nonce) - 1);
    item.sensor_mac[sizeof(item.sensor_mac) - 1] = '\0';
    item.nonce[sizeof(item.nonce) - 1] = '\0';

    if (xQueueSend(s_commit_queue, &item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "COMMIT queue full — COMMIT not queued for %s", sensor_mac);
        log_internal_heap("COMMIT queue full");
        return false;
    }

    ESP_LOGI(TAG, "COMMIT queued for %s", sensor_mac);
    return true;
}

// ── Receive callback ──────────────────────────────────────────────────────

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (data_len < (int)sizeof(espnow_packet_t)) return;

    espnow_packet_t *pkt = (espnow_packet_t *)data;
    const uint8_t   *src  = recv_info->src_addr;

    char mac_str[18];
    mac_bytes_to_str(src, mac_str);

    sensor_entry_t *entry = find_sensor_by_mac(src);

    if (pkt->type == PKT_ACK) {
        if (entry == NULL) {
            ESP_LOGW(TAG, "ACK from unknown sensor %s — ignoring", mac_str);
            return;
        }

        char safe_payload[128];
        memcpy(safe_payload, pkt->payload, sizeof(safe_payload) - 1);
        safe_payload[127] = '\0';

        char ack_hub[18] = {0};
        char ack_sensor[18] = {0};
        char ack_nonce[17] = {0};
        if (!parse_pair_payload(safe_payload, ack_hub, sizeof(ack_hub),
                                ack_sensor, sizeof(ack_sensor),
                                ack_nonce, sizeof(ack_nonce))) {
            ESP_LOGW(TAG, "Malformed ACK from %s — ignoring", mac_str);
            return;
        }
        if (strcmp(ack_hub, g_hub_mac) != 0 ||
            strcmp(ack_sensor, mac_str) != 0 ||
            strcmp(ack_nonce, entry->nonce) != 0) {
            ESP_LOGW(TAG,
                     "ACK proof mismatch from %s — ignoring (hub=%s expected=%s sensor=%s expected=%s nonce=%s expected=%s)",
                     mac_str, ack_hub, g_hub_mac, ack_sensor, mac_str,
                     ack_nonce, entry->nonce);
            return;
        }

        if (entry->paired) {
            ESP_LOGI(TAG, "Duplicate ACK from %s for nonce %s — ignoring UI/NVS side effects",
                     mac_str, ack_nonce);
            return;
        }

        ESP_LOGI(TAG, "Validated ACK from %s — ESP-NOW link established", mac_str);
        bool notify_on_ack = entry->notify_on_ack;
        if (!queue_commit_retry(entry, mac_str)) {
            ESP_LOGE(TAG, "ACK from %s validated but COMMIT could not be queued; keeping pairing provisional", mac_str);
            return;
        }

        entry->paired = true;
        entry->notify_on_ack = false;

        commit_sensor_table();
        nvs_prov_clear();

        if (!entry->is_reconnect) {
            event_item_t citem = {0};
            strncpy(citem.mac_str, mac_str, sizeof(citem.mac_str) - 1);
            citem.is_confirm = true;
            if (!s_event_queue || xQueueSend(s_event_queue, &citem, 0) != pdTRUE) {
                ESP_LOGE(TAG, "Event queue full — sensor %s will not be confirmed on server", mac_str);
            }
        }

        display_sensor_list();
        display_update_sensor_count();
        if (notify_on_ack) {
            char name[16];
            snprintf(name, sizeof(name), "S%d", sensor_index_from_entry(entry) + 1);
            display_sensor_added_notification(name);
            if (g_mode == MODE_SENSOR_PAIRING) {
                g_mode = MODE_OPERATIONAL;
            }
        }

        ESP_LOGI(TAG, "Sensor %s committed to hub NVS; total sensors=%d", mac_str, s_sensor_count);

    } else if (pkt->type == PKT_EVENT) {
        if (entry == NULL) {
            ESP_LOGW(TAG, "Event from unknown sensor %s — ignoring", mac_str);
            return;
        }

        ESP_LOGI(TAG, "Event received from %s", mac_str);
        if (!entry->enabled || g_mode == MODE_OFFLINE) {
            ESP_LOGI(TAG, "Dropping event from disabled/offline sensor %s", mac_str);
            return;
        }

        event_item_t item = {0};
        strncpy(item.mac_str, mac_str, sizeof(item.mac_str) - 1);
        strncpy(item.payload, pkt->payload, sizeof(item.payload) - 1);
        item.rssi = recv_info->rx_ctrl->rssi;

        if (!s_event_queue || xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Event queue full — dropping from %s", mac_str);
        } else {
            ESP_LOGI(TAG, "Event queued for API forwarding from %s", mac_str);
        }
    }
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    const uint8_t *mac_addr = (tx_info != NULL) ? tx_info->des_addr : NULL;

    if (mac_addr) {
        ESP_LOGI(TAG, "Send to %02X:%02X:%02X:%02X:%02X:%02X: %s",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5],
            status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
        return;
    }

    ESP_LOGI(TAG, "Send status: %s",
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

// ── HELLO retry task ──────────────────────────────────────────────────────
// Sends HELLO packets until ACK received or max_retries exhausted.
// max_retries=10 for initial pairing (30s), max_retries=20 for reconnect (60s).
// On failure during initial pairing: clears provisional NVS.

typedef struct {
    sensor_entry_t *entry;
    int             max_retries;
    bool            is_reconnect;  // true → don't clear prov NVS on failure
} hello_retry_arg_t;

static void hello_retry_task(void *arg)
{
    hello_retry_arg_t *a = (hello_retry_arg_t *)arg;
    sensor_entry_t *entry = a->entry;
    int max_retries       = a->max_retries;
    bool is_reconnect     = a->is_reconnect;
    free(a);

    uint8_t primary; wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);

    char mac_str[18];
    mac_bytes_to_str(entry->mac, mac_str);
    if (!is_reconnect || entry->nonce[0] == '\0') {
        make_nonce(entry->nonce);
    }

    espnow_packet_t pkt = { .type = PKT_HELLO };
    snprintf(pkt.payload, sizeof(pkt.payload), "H:%s;S:%s;N:%s;C:%u",
             g_hub_mac, mac_str, entry->nonce, primary);
    ESP_LOGI(TAG, "HELLO retry task started for %s: retries=%d reconnect=%d channel=%d",
             mac_str, max_retries, is_reconnect, primary);

    for (int i = 0; i < max_retries; i++) {
        if (entry->paired) break;

        esp_err_t err = esp_now_send(entry->mac, (uint8_t *)&pkt, sizeof(pkt));
        if (i == 0 || ((i + 1) % 10) == 0 || err != ESP_OK) {
            ESP_LOGI(TAG, "HELLO to %s attempt %d/%d: %s (ch%d)",
                     mac_str, i + 1, max_retries,
                     err == ESP_OK ? "sent" : esp_err_to_name(err), primary);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!entry->paired) {
        ESP_LOGE(TAG, "No ACK from %s after %d attempts", mac_str, max_retries);
        if (!is_reconnect) {
            ESP_LOGW(TAG, "Clearing provisional sensor after ACK timeout: %s", mac_str);
            nvs_prov_clear();   // roll back: sensor never confirmed
            remove_sensor_entry(entry);
            if (g_mode == MODE_SENSOR_PAIRING) {
                g_mode = MODE_OPERATIONAL;
            }
        }
    } else {
        ESP_LOGI(TAG, "HELLO retry task done for %s: paired", mac_str);
    }

    vTaskDelete(NULL);
}

static void start_hello_retry(sensor_entry_t *entry, int max_retries, bool is_reconnect)
{
    hello_retry_arg_t *arg = malloc(sizeof(hello_retry_arg_t));
    if (!arg) {
        ESP_LOGE(TAG, "OOM starting HELLO retry task");
        return;
    }
    arg->entry        = entry;
    arg->max_retries  = max_retries;
    arg->is_reconnect = is_reconnect;
    if (xTaskCreatePinnedToCore(hello_retry_task, "hello_retry", 3072, arg, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HELLO retry task");
        free(arg);
    }
}

// ── Reconnect manager: single task replaces N parallel hello_retry tasks ──
// Sends HELLO to all saved sensors every 200 ms in a tight burst.
// Each sensor gets the same HELLO cadence as the per-sensor approach but
// from one 3072-byte task instead of N × 3072 bytes of parallel tasks.

typedef struct {
    int  indices[MAX_SENSORS];
    int  count;
} reconnect_mgr_arg_t;

static void reconnect_manager_task(void *arg)
{
    reconnect_mgr_arg_t *a = (reconnect_mgr_arg_t *)arg;
    int  count = a->count;
    int  indices[MAX_SENSORS];
    memcpy(indices, a->indices, count * sizeof(int));
    free(a);

    uint8_t primary; wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);

    ESP_LOGI(TAG, "Reconnect manager: %d sensor(s), channel=%d, max 150 rounds", count, primary);

    for (int round = 0; round < 150; round++) {
        bool all_paired = true;
        for (int i = 0; i < count; i++) {
            sensor_entry_t *entry = &s_sensors[indices[i]];
            if (entry->paired) continue;
            all_paired = false;

            char mac_str[18];
            mac_bytes_to_str(entry->mac, mac_str);

            espnow_packet_t pkt = { .type = PKT_HELLO };
            snprintf(pkt.payload, sizeof(pkt.payload), "H:%s;S:%s;N:%s;C:%u",
                     g_hub_mac, mac_str, entry->nonce, primary);
            esp_now_send(entry->mac, (uint8_t *)&pkt, sizeof(pkt));
        }
        if (all_paired) {
            ESP_LOGI(TAG, "Reconnect manager: all sensors paired after %d rounds", round + 1);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    display_sensor_list();
    display_update_sensor_count();
    ESP_LOGI(TAG, "Reconnect manager task done");
    vTaskDelete(NULL);
}

// ── Public API ────────────────────────────────────────────────────────────

void espnow_init(void)
{
    if (s_espnow_ready) {
        ESP_LOGI(TAG, "ESP-NOW already initialized");
        return;
    }

    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_item_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        log_internal_heap("event queue create failed");
        return;
    }
    if (xTaskCreatePinnedToCore(event_forward_task, "evt_fwd", 6144, NULL, 4, &s_event_task_handle, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create event forward task");
        log_internal_heap("event forward task create failed");
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return;
    }

    s_commit_queue = xQueueCreate(COMMIT_QUEUE_DEPTH, sizeof(commit_item_t));
    if (!s_commit_queue) {
        ESP_LOGE(TAG, "Failed to create COMMIT queue");
        log_internal_heap("COMMIT queue create failed");
        vTaskDelete(s_event_task_handle);
        s_event_task_handle = NULL;
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return;
    }
    if (xTaskCreatePinnedToCore(commit_retry_worker_task, "commit_retry", COMMIT_WORKER_STACK,
                                NULL, 5, &s_commit_task_handle, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create COMMIT retry worker task");
        log_internal_heap("COMMIT worker task create failed");
        vQueueDelete(s_commit_queue);
        s_commit_queue = NULL;
        vTaskDelete(s_event_task_handle);
        s_event_task_handle = NULL;
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return;
    }

    ESP_LOGI(TAG, "Initializing ESP-NOW stack");
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)GLAZIA_ESP_NOW_PMK));
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    uint8_t primary; wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    s_espnow_ready = true;
    ESP_LOGI(TAG, "ESP-NOW ready — channel: %d, PMK set", primary);
}

void espnow_deinit(void)
{
    if (!s_espnow_ready) return;

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_err_t err = esp_now_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_deinit failed: %s", esp_err_to_name(err));
    }

    if (s_commit_task_handle) {
        vTaskDelete(s_commit_task_handle);
        s_commit_task_handle = NULL;
    }
    if (s_commit_queue) {
        vQueueDelete(s_commit_queue);
        s_commit_queue = NULL;
    }
    if (s_event_task_handle) {
        vTaskDelete(s_event_task_handle);
        s_event_task_handle = NULL;
    }
    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }

    memset(s_sensors, 0, sizeof(s_sensors));
    s_sensor_count = 0;
    s_espnow_ready = false;
    ESP_LOGI(TAG, "ESP-NOW stopped");
}

void espnow_pair_sensor(const char *sensor_mac_str, const char *provision_key_hex,
                        const char *name, const char *zone)
{
    if (s_sensor_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "MAX_SENSORS (%d) reached", MAX_SENSORS);
        nvs_prov_clear();
        return;
    }

    uint8_t sensor_mac[6];
    mac_str_to_bytes(sensor_mac_str, sensor_mac);
    sensor_entry_t *entry = find_sensor_by_mac(sensor_mac);
    if (entry && entry->paired) {
        ESP_LOGW(TAG, "Sensor %s already paired — ignoring duplicate pair request", sensor_mac_str);
        nvs_prov_clear();
        return;
    }

    ESP_LOGI(TAG, "Pairing sensor #%d: %s", entry ? sensor_index_from_entry(entry) + 1 : s_sensor_count + 1, sensor_mac_str);

    if (!entry) {
        entry = &s_sensors[s_sensor_count];
        memcpy(entry->mac, sensor_mac, 6);
        s_sensor_count++;
    }
    hex_to_bytes(provision_key_hex, entry->lmk, 16);
    entry->paired = false;
    entry->enabled = true;
    entry->is_reconnect = false;
    entry->notify_on_ack = true;
    strncpy(entry->name, name ? name : "", sizeof(entry->name) - 1);
    strncpy(entry->zone, zone ? zone : "", sizeof(entry->zone) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->zone[sizeof(entry->zone) - 1] = '\0';
    make_nonce(entry->nonce);

    if (esp_now_is_peer_exist(entry->mac)) {
        esp_now_del_peer(entry->mac);
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, entry->mac, 6);
    peer.channel = 0;
    peer.encrypt = true;
    memcpy(peer.lmk, entry->lmk, 16);

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add encrypted peer %s: %s", sensor_mac_str, esp_err_to_name(err));
        remove_sensor_entry(entry);
        nvs_prov_clear();
        return;
    }
    ESP_LOGI(TAG, "Encrypted ESP-NOW peer added: %s", sensor_mac_str);

    start_hello_retry(entry, 300, false);
}

void espnow_get_sensor_list_str(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (s_sensor_count == 0) return;

    size_t pos = 0;
    for (int i = 0; i < s_sensor_count; i++) {
        const char *sep = (i < s_sensor_count - 1) ? ";" : "";
        int n = snprintf(out + pos, out_len - pos,
                         "S%d|%s|%s%s",
                         i + 1,
                         s_sensors[i].zone[0] ? s_sensors[i].zone :
                         (s_sensors[i].name[0] ? s_sensors[i].name : "Unknown"),
                         s_sensors[i].enabled ? "ON" : "OFF",
                         sep);
        if (n < 0 || (size_t)n >= out_len - pos) break;
        pos += (size_t)n;
    }
}

void espnow_reconnect_saved_sensors(void)
{
    char    (*macs)[18] = malloc(MAX_SENSORS * sizeof(*macs));
    uint8_t (*keys)[16] = malloc(MAX_SENSORS * sizeof(*keys));
    char    (*names)[32] = malloc(MAX_SENSORS * sizeof(*names));
    char    (*zones)[32] = malloc(MAX_SENSORS * sizeof(*zones));
    if (!macs || !keys || !names || !zones) {
        ESP_LOGE(TAG, "OOM in espnow_reconnect_saved_sensors");
        free(macs); free(keys); free(names); free(zones);
        return;
    }
    int count = nvs_load_sensors(macs, keys, names, zones, MAX_SENSORS);

    if (count == 0) {
        ESP_LOGI(TAG, "No saved sensors — nothing to reconnect");
        free(macs);
        free(keys);
        free(names);
        free(zones);
        return;
    }

    ESP_LOGI(TAG, "Reconnecting %d saved sensor(s)...", count);
    for (int i = 0; i < count; i++) {
        if (s_sensor_count >= MAX_SENSORS) break;
        ESP_LOGI(TAG, "  → %s (sending HELLO to re-establish link)", macs[i]);

        sensor_entry_t *entry = &s_sensors[s_sensor_count];
        mac_str_to_bytes(macs[i], entry->mac);
        memcpy(entry->lmk, keys[i], 16);
        entry->paired = false;   // will be set true on ACK
        entry->enabled = nvs_load_sensor_enabled(i, true);
        entry->is_reconnect = true;
        entry->notify_on_ack = false;
        strncpy(entry->name, names[i], sizeof(entry->name) - 1);
        strncpy(entry->zone, zones[i], sizeof(entry->zone) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        entry->zone[sizeof(entry->zone) - 1] = '\0';
        make_nonce(entry->nonce);
        s_sensor_count++;

        if (!esp_now_is_peer_exist(entry->mac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, entry->mac, 6);
            peer.channel = 0;
            peer.encrypt = true;
            memcpy(peer.lmk, entry->lmk, 16);

            esp_err_t err = esp_now_add_peer(&peer);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to re-add peer %s: %s", macs[i], esp_err_to_name(err));
                s_sensor_count--;
                continue;
            }
            ESP_LOGI(TAG, "Encrypted ESP-NOW peer restored: %s", macs[i]);
        } else {
            ESP_LOGI(TAG, "Saved ESP-NOW peer already exists: %s", macs[i]);
        }

    }

    free(macs);
    free(keys);
    free(names);
    free(zones);

    // Push initial sensor list to display (all OFF until ACK received)
    display_sensor_list();

    if (s_sensor_count == 0) return;

    // Spawn a single reconnect manager instead of one hello_retry task per sensor.
    // All sensors get HELLO every 200 ms from one task (3072 B) rather than
    // N tasks × 3072 B running in parallel for 30 s.
    reconnect_mgr_arg_t *mgr_arg = malloc(sizeof(reconnect_mgr_arg_t));
    if (!mgr_arg) {
        ESP_LOGE(TAG, "OOM for reconnect manager — falling back to per-sensor tasks");
        for (int i = 0; i < s_sensor_count; i++) {
            start_hello_retry(&s_sensors[i], 150, true);
        }
        return;
    }
    mgr_arg->count = s_sensor_count;
    for (int i = 0; i < s_sensor_count; i++) {
        mgr_arg->indices[i] = i;
    }
    if (xTaskCreatePinnedToCore(reconnect_manager_task, "reconnect_mgr", 3072,
                                mgr_arg, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reconnect manager task — falling back to per-sensor tasks");
        free(mgr_arg);
        for (int i = 0; i < s_sensor_count; i++) {
            start_hello_retry(&s_sensors[i], 150, true);
        }
    }
}

int espnow_get_sensor_count(void)
{
    return s_sensor_count;
}

int espnow_get_active_sensor_count(void)
{
    int count = 0;
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].enabled && s_sensors[i].paired) count++;
    }
    return count;
}

bool espnow_get_sensor_info(int index, char *out_name, size_t out_name_len, bool *out_enabled, bool *out_paired)
{
    if (index < 0 || index >= s_sensor_count) return false;
    if (out_name && out_name_len > 0) {
        const char *label = s_sensors[index].zone[0] ? s_sensors[index].zone :
                            (s_sensors[index].name[0] ? s_sensors[index].name : NULL);
        if (label) {
            snprintf(out_name, out_name_len, "%s", label);
        } else {
            snprintf(out_name, out_name_len, "S%d", index + 1);
        }
    }
    if (out_enabled) *out_enabled = s_sensors[index].enabled;
    if (out_paired) *out_paired = s_sensors[index].paired;
    return true;
}

void espnow_set_sensor_enabled(int index, bool enabled)
{
    if (index < 0 || index >= s_sensor_count) return;
    sensor_entry_t *entry = &s_sensors[index];

    if (entry->enabled == enabled) return;

    entry->enabled = enabled;
    nvs_save_sensor_enabled(index, enabled);

    if (!enabled) {
        esp_now_del_peer(entry->mac);
        entry->paired = false;
        ESP_LOGI(TAG, "Sensor S%d disabled and disconnected", index + 1);
    } else {
        if (!esp_now_is_peer_exist(entry->mac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, entry->mac, 6);
            peer.channel = 0;
            peer.encrypt = true;
            memcpy(peer.lmk, entry->lmk, 16);
            esp_err_t err = esp_now_add_peer(&peer);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to re-add peer for S%d: %s", index + 1, esp_err_to_name(err));
                entry->enabled = false;
                nvs_save_sensor_enabled(index, false);
                return;
            }
        }
        entry->is_reconnect = true;
        entry->notify_on_ack = false;
        entry->nonce[0] = '\0';
        start_hello_retry(entry, 150, true);
        ESP_LOGI(TAG, "Sensor S%d re-enabled, reconnect started", index + 1);
    }

    display_sensor_list();
    display_update_sensor_count();
}

esp_err_t espnow_set_sensor_enabled_by_mac(const char *mac_str, bool enabled)
{
    uint8_t mac[6];
    mac_str_to_bytes(mac_str, mac);
    sensor_entry_t *entry = find_sensor_by_mac(mac);
    if (!entry) return ESP_ERR_NOT_FOUND;
    espnow_set_sensor_enabled((int)(entry - s_sensors), enabled);
    return ESP_OK;
}

static void send_pkt_reset(const uint8_t *mac)
{
    espnow_packet_t pkt = { .type = PKT_RESET };
    pkt.payload[0] = '\0';
    char mac_str[18];
    mac_bytes_to_str(mac, mac_str);
    esp_err_t err = esp_now_send(mac, (uint8_t *)&pkt, sizeof(pkt));
    ESP_LOGI(TAG, "PKT_RESET to %s: %s", mac_str, err == ESP_OK ? "queued" : esp_err_to_name(err));
}

esp_err_t espnow_remove_sensor(const char *sensor_mac_str)
{
    uint8_t mac[6];
    mac_str_to_bytes(sensor_mac_str, mac);
    sensor_entry_t *entry = find_sensor_by_mac(mac);
    if (!entry) {
        ESP_LOGW(TAG, "espnow_remove_sensor: %s not in peer table", sensor_mac_str);
        return ESP_ERR_NOT_FOUND;
    }

    if (entry->paired) {
        send_pkt_reset(entry->mac);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    remove_sensor_entry(entry);
    commit_sensor_table();
    ESP_LOGI(TAG, "Sensor %s removed from peer table and NVS; remaining=%d",
             sensor_mac_str, s_sensor_count);
    return ESP_OK;
}

void espnow_send_reset_to_all_sensors(void)
{
    for (int i = 0; i < s_sensor_count; i++) {
        if (s_sensors[i].paired) {
            send_pkt_reset(s_sensors[i].mac);
        }
    }
    if (s_sensor_count > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void espnow_queue_hub_event(const char *event_type)
{
    if (!s_event_queue) {
        ESP_LOGW(TAG, "Hub event '%s' dropped — event queue not ready", event_type);
        return;
    }
    event_item_t item = {0};
    item.is_hub_event = true;
    strncpy(item.payload, event_type, sizeof(item.payload) - 1);
    if (xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Hub event queue full — dropping '%s'", event_type);
    }
}
