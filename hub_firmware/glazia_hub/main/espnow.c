#include "espnow.h"
#include "state.h"
#include "display.h"
#include "api_client.h"
#include "nvs_storage.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ESPNOW";

// ── Packet types ──────────────────────────────────────────────────────────
#define PKT_HELLO   0x01
#define PKT_ACK     0x02
#define PKT_EVENT   0x03

typedef struct {
    uint8_t type;
    char    payload[128];
} espnow_packet_t;

// ── Event forwarding queue ─────────────────────────────────────────────────
#define EVENT_QUEUE_DEPTH 20

typedef struct {
    char mac_str[18];
    char payload[128];
} event_item_t;

static QueueHandle_t s_event_queue = NULL;

static void event_forward_task(void *arg)
{
    event_item_t item;
    while (1) {
        if (xQueueReceive(s_event_queue, &item, portMAX_DELAY) == pdTRUE) {
            api_send_event(item.mac_str, item.payload, "info");
        }
    }
}

// ── Multi-sensor table ────────────────────────────────────────────────────
#define MAX_SENSORS 20

typedef struct {
    uint8_t mac[6];
    uint8_t lmk[16];   // LMK = provision_key bytes
    bool    paired;    // true once ACK received
} sensor_entry_t;

static sensor_entry_t s_sensors[MAX_SENSORS];
static int            s_sensor_count = 0;

// ── Helpers ───────────────────────────────────────────────────────────────

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

// ── Receive callback ──────────────────────────────────────────────────────

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (data_len < (int)sizeof(espnow_packet_t)) return;

    espnow_packet_t *pkt = (espnow_packet_t *)data;
    const uint8_t   *src  = recv_info->src_addr;

    char mac_str[18];
    mac_bytes_to_str(src, mac_str);

    sensor_entry_t *entry = NULL;
    for (int i = 0; i < s_sensor_count; i++) {
        if (memcmp(s_sensors[i].mac, src, 6) == 0) {
            entry = &s_sensors[i];
            break;
        }
    }

    if (pkt->type == PKT_ACK) {
        if (entry == NULL) {
            ESP_LOGW(TAG, "ACK from unknown sensor %s — ignoring", mac_str);
            return;
        }

        ESP_LOGI(TAG, "ACK from %s — ESP-NOW link established!", mac_str);
        entry->paired = true;

        display_show("Sensor Paired!", mac_str);

        // Clear provisional NVS (may already be empty on reconnect — that's fine)
        nvs_prov_clear();

        // Re-persist full sensor table with LMK keys
        char    saved_macs[MAX_SENSORS][18];
        uint8_t saved_keys[MAX_SENSORS][16];
        int     saved_count = 0;
        for (int i = 0; i < s_sensor_count; i++) {
            mac_bytes_to_str(s_sensors[i].mac, saved_macs[saved_count]);
            memcpy(saved_keys[saved_count], s_sensors[i].lmk, 16);
            saved_count++;
        }
        nvs_save_sensors(saved_macs, saved_keys, saved_count);

    } else if (pkt->type == PKT_EVENT) {
        if (entry == NULL) {
            ESP_LOGW(TAG, "Event from unknown sensor %s — ignoring", mac_str);
            return;
        }

        ESP_LOGI(TAG, "Event from %s: %s", mac_str, pkt->payload);
        display_show("SENSOR DATA", pkt->payload);

        event_item_t item;
        strncpy(item.mac_str, mac_str, sizeof(item.mac_str) - 1);
        strncpy(item.payload, pkt->payload, sizeof(item.payload) - 1);
        item.mac_str[sizeof(item.mac_str) - 1] = '\0';
        item.payload[sizeof(item.payload) - 1]  = '\0';

        if (xQueueSend(s_event_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Event queue full — dropping from %s", mac_str);
        }
    }
}

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "Send to %02X:%02X:%02X:%02X:%02X:%02X: %s",
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5],
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

    espnow_packet_t pkt = { .type = PKT_HELLO };
    snprintf(pkt.payload, sizeof(pkt.payload), "HELLO_FROM_HUB:ch=%d", primary);

    char mac_str[18];
    mac_bytes_to_str(entry->mac, mac_str);

    for (int i = 0; i < max_retries; i++) {
        if (entry->paired) break;

        esp_err_t err = esp_now_send(entry->mac, (uint8_t *)&pkt, sizeof(pkt));
        ESP_LOGI(TAG, "HELLO → %s  attempt %d/%d: %s (ch%d)",
            mac_str, i + 1, max_retries,
            err == ESP_OK ? "sent" : esp_err_to_name(err), primary);

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!entry->paired) {
        ESP_LOGE(TAG, "No ACK from %s after %d attempts", mac_str, max_retries);
        display_show("Pair Failed", "No response");
        if (!is_reconnect) {
            nvs_prov_clear();   // roll back: sensor never confirmed
        }
    }

    vTaskDelete(NULL);
}

static void start_hello_retry(sensor_entry_t *entry, int max_retries, bool is_reconnect)
{
    hello_retry_arg_t *arg = malloc(sizeof(hello_retry_arg_t));
    if (!arg) return;
    arg->entry        = entry;
    arg->max_retries  = max_retries;
    arg->is_reconnect = is_reconnect;
    xTaskCreate(hello_retry_task, "hello_retry", 3072, arg, 5, NULL);
}

// ── Public API ────────────────────────────────────────────────────────────

void espnow_init(void)
{
    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_item_t));
    xTaskCreate(event_forward_task, "evt_fwd", 4096, NULL, 4, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)GLAZIA_ESP_NOW_PMK));
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    uint8_t primary; wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    ESP_LOGI(TAG, "ESP-NOW ready — channel: %d, PMK set", primary);
}

void espnow_pair_sensor(const char *sensor_mac_str, const char *provision_key_hex)
{
    if (s_sensor_count >= MAX_SENSORS) {
        ESP_LOGE(TAG, "MAX_SENSORS (%d) reached", MAX_SENSORS);
        display_show("Pair Failed", "Max sensors!");
        nvs_prov_clear();
        return;
    }

    ESP_LOGI(TAG, "Pairing sensor #%d: %s", s_sensor_count + 1, sensor_mac_str);
    display_show("Sensor Found!", sensor_mac_str);

    sensor_entry_t *entry = &s_sensors[s_sensor_count];
    mac_str_to_bytes(sensor_mac_str, entry->mac);
    hex_to_bytes(provision_key_hex, entry->lmk, 16);
    entry->paired = false;
    s_sensor_count++;

    if (!esp_now_is_peer_exist(entry->mac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, entry->mac, 6);
        peer.channel = 0;
        peer.encrypt = true;
        memcpy(peer.lmk, entry->lmk, 16);

        if (esp_now_add_peer(&peer) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add encrypted peer %s", sensor_mac_str);
            s_sensor_count--;
            nvs_prov_clear();
            return;
        }
    }

    start_hello_retry(entry, 300, false);
}

void espnow_reconnect_saved_sensors(void)
{
    char    macs[MAX_SENSORS][18];
    uint8_t keys[MAX_SENSORS][16];
    int count = nvs_load_sensors(macs, keys, MAX_SENSORS);

    if (count == 0) {
        ESP_LOGI(TAG, "No saved sensors — nothing to reconnect");
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
        s_sensor_count++;

        if (!esp_now_is_peer_exist(entry->mac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, entry->mac, 6);
            peer.channel = 0;
            peer.encrypt = true;
            memcpy(peer.lmk, entry->lmk, 16);

            if (esp_now_add_peer(&peer) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to re-add peer %s", macs[i]);
                s_sensor_count--;
                continue;
            }
        }

        // Send HELLO to re-establish the encrypted channel after reboot.
        // 20 retries (60s) to account for sensor boot time.
        start_hello_retry(entry, 150, true);
    }
}
