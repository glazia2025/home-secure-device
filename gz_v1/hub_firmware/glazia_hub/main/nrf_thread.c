#include "nrf_thread.h"
#include "nrf_ipc.h"
#include "nvs_storage.h"
#include "api_client.h"
#include "state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "NRF_THR";

volatile bool g_thread_net_ready = false;
static volatile bool s_ipc_alive  = false;

/* ── EUI64 helpers ────────────────────────────────────────────────────────── */
static void eui64_to_hex(const uint8_t eui64[8], char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", eui64[i]);
}

/* ── IPC event handler ────────────────────────────────────────────────────── */
static void on_ipc_event(uint8_t type, const uint8_t *payload, uint16_t len)
{
    switch (type) {

    case IPC_EVT_PONG:
        if (!s_ipc_alive) {
            s_ipc_alive = true;
            ESP_LOGI(TAG, "UART handshake OK — nRF is alive");
        }
        break;

    case IPC_EVT_NET_UP: {
        if (len < 3) break;
        uint8_t  channel = payload[0];
        uint16_t panid   = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
        ESP_LOGI(TAG, "Thread net up: channel=%u PAN=0x%04x", channel, panid);
        g_thread_net_ready = true;
        break;
    }

    case IPC_EVT_NET_DOWN:
        ESP_LOGW(TAG, "Thread net down");
        g_thread_net_ready = false;
        break;

    case IPC_EVT_SENSOR_JOINED: {
        if (len < 8) break;
        char hex[17];
        eui64_to_hex(payload, hex);
        ESP_LOGI(TAG, "Sensor joined Thread: eui64=%s", hex);

        /* Load existing Thread sensors, append new one, save back */
        uint8_t eui64s[10][8];
        char    names[10][32];
        char    zones[10][32];
        int     count = nvs_load_thread_sensors(eui64s, names, zones, 10);

        /* Skip if already stored */
        for (int i = 0; i < count; i++) {
            if (memcmp(eui64s[i], payload, 8) == 0) {
                ESP_LOGI(TAG, "Sensor eui64=%s already in NVS", hex);
                goto joined_confirm;
            }
        }

        if (count < 10) {
            memcpy(eui64s[count], payload, 8);
            snprintf(names[count], sizeof(names[count]), "Sensor-%s", hex + 8);
            zones[count][0] = '\0';
            count++;
            nvs_save_thread_sensors(eui64s, names, zones, count);
        } else {
            ESP_LOGW(TAG, "Thread sensor table full (10), dropping %s", hex);
        }

joined_confirm:
        /* Notify server — reuse api_confirm_sensor with hex EUI64 as identifier.
         * Server-side schema update (eui64 vs MAC) is tracked separately. */
        api_confirm_sensor(hex);
        break;
    }

    case IPC_EVT_SENSOR_DATA: {
        if (len < 9) break;          /* need at least eui64 + 1 byte JSON */
        char hex[17];
        eui64_to_hex(payload, hex);

        /* JSON payload starts after the 8-byte EUI64 */
        const char *json = (const char *)(payload + 8);
        ESP_LOGI(TAG, "Sensor data from %s: %s", hex, json);

        /* Parse {"e":"<event_name>"} and forward to cloud */
        char event_buf[32] = {0};
        const char *e_start = strstr(json, "\"e\":\"");
        if (e_start) {
            e_start += 5;
            const char *e_end = strchr(e_start, '"');
            if (e_end && (e_end - e_start) < (int)sizeof(event_buf)) {
                memcpy(event_buf, e_start, e_end - e_start);
                event_buf[e_end - e_start] = '\0';
            }
        }

        if (event_buf[0]) {
            /* Map sensor event names to API event types */
            const char *event_type = event_buf;
            if (strcmp(event_buf, "door_open") == 0)       event_type = "door_opened";
            else if (strcmp(event_buf, "door_close") == 0) event_type = "door_closed";
            /* "vibration" passes through unchanged */

            api_send_event(hex, event_type, "normal", "{}");
        } else {
            ESP_LOGW(TAG, "Could not parse event from: %s", json);
        }
        break;
    }

    case IPC_EVT_COMM_FAILED: {
        if (len < 8) break;
        char hex[17];
        eui64_to_hex(payload, hex);
        ESP_LOGW(TAG, "Thread commissioning failed for eui64=%s", hex);
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown IPC event 0x%02x", type);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Boot handshake: wait for the nRF to finish booting (its UART is not ready until ~3.8 s),
 * then a short fixed burst of PINGs, then stop. Starting at ~4 s means all 5 pings hit a
 * listening nRF (5 PONG chances, not ~2). The first PONG latches s_ipc_alive (logged once in
 * on_ipc_event). No keepalive, no reprobe: after the burst the hub goes silent. */
#define IPC_HANDSHAKE_START_MS 2500   /* task starts ~1.6 s after boot; +2.5 s => 1st ping ~4 s */
#define IPC_HANDSHAKE_PINGS    5
#define IPC_HANDSHAKE_GAP_MS   1000

static void ipc_handshake_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(IPC_HANDSHAKE_START_MS));   /* let the nRF finish booting */
    for (int i = 1; i <= IPC_HANDSHAKE_PINGS; i++) {
        ipc_cmd_ping();
        ESP_LOGI(TAG, "handshake PING %d/%d", i, IPC_HANDSHAKE_PINGS);
        vTaskDelay(pdMS_TO_TICKS(IPC_HANDSHAKE_GAP_MS));
    }
    if (s_ipc_alive) {
        ESP_LOGI(TAG, "nRF handshake complete — link up");
    } else {
        ESP_LOGW(TAG, "nRF handshake done — no PONG (check nRF power/wiring)");
    }
    vTaskDelete(NULL);
}

/* The nRF forms its Thread network autonomously at its own boot (a self-contained 802.15.4
 * network, independent of WiFi and the hub). The hub does NOT drive formation — it only learns
 * the state: once the UART link is up, poll CMD_NET_STATUS until the nRF reports NET_UP. This is
 * a read-only query (never changes nRF state, unlike NET_FORM), so it's safe to repeat — and it
 * also covers a hub-only reset while the nRF is already up (no role change → no async NET_UP),
 * plus a NET_UP frame lost to EMI. */
#define NET_STATUS_POLL_MS   3000
#define NET_STATUS_MAX_TRIES 10

static void net_watch_task(void *arg)
{
    while (!s_ipc_alive) vTaskDelay(pdMS_TO_TICKS(100));

    for (int i = 0; i < NET_STATUS_MAX_TRIES && !g_thread_net_ready; i++) {
        ipc_cmd_net_status();
        for (int t = 0; t < NET_STATUS_POLL_MS / 100 && !g_thread_net_ready; t++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (g_thread_net_ready)
        ESP_LOGI(TAG, "Thread network confirmed up");
    else
        ESP_LOGW(TAG, "Thread network not up after %d s of status polls — check nRF",
                 (NET_STATUS_POLL_MS * NET_STATUS_MAX_TRIES) / 1000);
    vTaskDelete(NULL);
}

void nrf_thread_preinit(void)
{
    nrf_ipc_init(on_ipc_event);
    ESP_LOGI(TAG, "nRF IPC UART ready at boot (RX=GPIO%d TX=GPIO%d)", NRF_UART_RX_GPIO, NRF_UART_TX_GPIO);
    /* Fire the handshake burst at boot, decoupled from WiFi. */
    xTaskCreate(ipc_handshake_task, "ipc_hs", 3072, NULL, 3, NULL);
    /* Passive watcher: confirms the nRF's autonomous Thread network came up. */
    xTaskCreate(net_watch_task, "net_watch", 3072, NULL, 3, NULL);
}

void nrf_thread_on_wifi_ready(void)
{
    /* Thread formation is autonomous on the nRF and independent of WiFi — nothing to do here. */
    ESP_LOGI(TAG, "WiFi ready (Thread network is formed autonomously by the nRF)");
}

void nrf_thread_commission_sensor(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s)
{
    ipc_cmd_commission(eui64, pskd, timeout_s);
}

void nrf_thread_delete_sensor(const uint8_t eui64[8])
{
    ipc_cmd_sensor_del(eui64);

    /* Remove from NVS */
    uint8_t eui64s[10][8];
    char    names[10][32];
    char    zones[10][32];
    int     count = nvs_load_thread_sensors(eui64s, names, zones, 10);
    int     new_count = 0;
    for (int i = 0; i < count; i++) {
        if (memcmp(eui64s[i], eui64, 8) != 0) {
            if (new_count != i) {
                memcpy(eui64s[new_count], eui64s[i], 8);
                memcpy(names[new_count], names[i], 32);
                memcpy(zones[new_count], zones[i], 32);
            }
            new_count++;
        }
    }
    nvs_save_thread_sensors(eui64s, names, zones, new_count);
}
