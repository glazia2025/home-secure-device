#include "espnow.h"
#include "state.h"
#include "display.h"
#include "api_client.h"
#include "websocket.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ESPNOW";

// Paired sensor MAC (binary)
static uint8_t s_sensor_mac[6] = {0};
static bool    s_sensor_paired  = false;

// ── Packet types ──────────────────────────────────────────────────────────
#define PKT_HELLO   0x01
#define PKT_ACK     0x02
#define PKT_EVENT   0x03

typedef struct {
    uint8_t type;
    char    payload[128];
} espnow_packet_t;

// ── MAC string → binary ───────────────────────────────────────────────────

static void mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes)
{
    sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &mac_bytes[0], &mac_bytes[1], &mac_bytes[2],
        &mac_bytes[3], &mac_bytes[4], &mac_bytes[5]);
}

// ── Receive callback ──────────────────────────────────────────────────────

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (data_len < sizeof(espnow_packet_t)) return;

    espnow_packet_t *pkt = (espnow_packet_t *)data;

    if (pkt->type == PKT_ACK) {
        ESP_LOGI(TAG, "Got ACK from sensor — ESP-NOW link established!");
        s_sensor_paired = true;

        // Show on display
        display_show("Sensor Paired!", "System Active");

        // Tell server ESP-NOW is done
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            s_sensor_mac[0], s_sensor_mac[1], s_sensor_mac[2],
            s_sensor_mac[3], s_sensor_mac[4], s_sensor_mac[5]);
        api_acknowledge_sensor(mac_str);

        // Close WebSocket
        websocket_stop();

        g_mode = MODE_OPERATIONAL;

    } else if (pkt->type == PKT_EVENT) {
        ESP_LOGI(TAG, "Sensor event: %s", pkt->payload);

        // Parse event payload
        cJSON *root = cJSON_Parse(pkt->payload);
        if (root) {
            cJSON *event_type = cJSON_GetObjectItem(root, "eventType");
            cJSON *severity   = cJSON_GetObjectItem(root, "severity");

            if (event_type) {
                char mac_str[18];
                snprintf(mac_str, sizeof(mac_str),
                    "%02X:%02X:%02X:%02X:%02X:%02X",
                    s_sensor_mac[0], s_sensor_mac[1], s_sensor_mac[2],
                    s_sensor_mac[3], s_sensor_mac[4], s_sensor_mac[5]);

                // Show on display
                display_show("ALERT!", event_type->valuestring);

                // Forward to server
                api_send_event(mac_str,
                    event_type->valuestring,
                    severity ? severity->valuestring : "info");
            }
            cJSON_Delete(root);
        }
    }
}

// ── Send callback (for debug) ─────────────────────────────────────────────

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    ESP_LOGI(TAG, "Send to sensor: %s",
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAILED");
}

// ── Public API ────────────────────────────────────────────────────────────

void espnow_init(void)
{
    esp_now_init();
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);
    ESP_LOGI(TAG, "ESP-NOW initialised");
}

void espnow_pair_sensor(const char *sensor_mac_str)
{
    ESP_LOGI(TAG, "Pairing with sensor: %s", sensor_mac_str);
    display_show("Connecting...", sensor_mac_str);

    mac_str_to_bytes(sensor_mac_str, s_sensor_mac);

    // Add sensor as ESP-NOW peer
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_sensor_mac, 6);
    peer.channel = 0;
    peer.encrypt = false;

    if (esp_now_add_peer(&peer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer");
        return;
    }

    // Send HELLO packet
    espnow_packet_t pkt = {
        .type = PKT_HELLO,
    };
    snprintf(pkt.payload, sizeof(pkt.payload), "HELLO_FROM_HUB");

    esp_err_t err = esp_now_send(s_sensor_mac, (uint8_t *)&pkt, sizeof(pkt));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HELLO sent to sensor — waiting for ACK");
    } else {
        ESP_LOGE(TAG, "Failed to send HELLO: %s", esp_err_to_name(err));
    }
}
