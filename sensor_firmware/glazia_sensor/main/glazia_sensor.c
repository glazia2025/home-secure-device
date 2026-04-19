/**
 * glazia_sensor.c — Glazia Sensor Node (ESP32-C3)
 *
 * Pairing flow (fresh sensor):
 *   1. Press button on GPIO6 → BLE advertising "GlaziaSensor" (60s)
 *   2. Phone/app writes hub_mac (char 0xFF10) and provision_key hex (char 0xFF11)
 *   3. Save to provisional NVS "glz_prov", stop BLE
 *   4. Init WiFi → ESP-NOW (set PMK), add hub as encrypted peer (LMK = provision_key)
 *   5. Channel-scan and wait up to 60s for HELLO from hub
 *   6. HELLO → lock channel, send ACK, promote "glz_prov" → "glz_main"
 *   7. Start periodic sensor event loop
 *
 * On reboot with "glz_main" NVS:
 *   Skip BLE. Init WiFi/ESP-NOW, add hub as encrypted peer, channel-scan
 *   indefinitely until hub's HELLO re-establishes the link, then send events.
 *
 * On reboot with "glz_prov" NVS (interrupted pairing):
 *   Skip BLE. Retry step 4-6 with the stored provisional credentials.
 *   On timeout: erase "glz_prov" and restart so the user can try again.
 *
 * PMK: compile-time constant — must match hub firmware exactly.
 * LMK: provision_key delivered out-of-band (BLE → server → hub HTTP → ESP-NOW).
 *       Never transmitted over the air; AES-128 encrypts every ESP-NOW frame.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "SENSOR";

/* ── Constants ────────────────────────────────────────────────────────────── */
#define BUTTON_GPIO            6
#define BLE_ADV_TIMEOUT_MS     (60 * 1000)
#define HELLO_PROV_TIMEOUT_MS  (10 * 60 * 1000)
#define NVS_MAIN_NS            "glz_main"
#define NVS_PROV_NS            "glz_prov"
// PMK must match hub firmware's GLAZIA_ESP_NOW_PMK (exactly 16 bytes)
#define GLAZIA_ESP_NOW_PMK     "glz!dev.pmk.2024"

#define PKT_HELLO  0x01
#define PKT_ACK    0x02
#define PKT_EVENT  0x03

typedef struct {
    uint8_t type;
    char    payload[128];
} __attribute__((packed)) espnow_packet_t;

/* ── Sensor state ─────────────────────────────────────────────────────────── */
static char    s_own_mac_str[18]  = {0};
static uint8_t s_hub_mac_bytes[6] = {0};
static uint8_t s_lmk[16]         = {0};
static volatile bool s_hub_paired = false;
static volatile bool s_scan_stop  = false;

/* ── BLE state ────────────────────────────────────────────────────────────── */
static SemaphoreHandle_t s_ble_done_sem   = NULL;
static volatile bool     s_ble_got_creds  = false;
static char              s_ble_hub_mac[18]  = {0};
static char              s_ble_prov_key[33] = {0};
static uint16_t          s_ble_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_hub_mac_val_handle  = 0;
static uint16_t          s_prov_key_val_handle = 0;

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static void hex_to_bytes(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        out[i] = (uint8_t)strtol(b, NULL, 16);
    }
}

static void mac_str_to_bytes(const char *s, uint8_t *b)
{
    sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
}

/* ── NVS helpers ──────────────────────────────────────────────────────────── */
static void nvs_save_pair(const char *ns, const char *hub_mac, const char *prov_key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "hub_mac",  hub_mac);
    nvs_set_str(h, "prov_key", prov_key);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS[%s] saved: hub=%s", ns, hub_mac);
}

static bool nvs_load_pair(const char *ns, char *hub_mac, char *prov_key)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 18;
    bool ok = (nvs_get_str(h, "hub_mac",  hub_mac,  &len) == ESP_OK);
    len = 33;
    ok  = ok && (nvs_get_str(h, "prov_key", prov_key, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

static void nvs_erase_ns(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS[%s] erased", ns);
    }
}

/* ── WiFi init ────────────────────────────────────────────────────────────── */
static void wifi_init_for_espnow(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    ESP_LOGI(TAG, "WiFi STA started for ESP-NOW");
}

/* ── ESP-NOW ──────────────────────────────────────────────────────────────── */
static void espnow_send_cb(const uint8_t *mac, esp_now_send_status_t status)
{
    ESP_LOGD(TAG, "Send %s", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (len < (int)sizeof(espnow_packet_t)) return;
    const espnow_packet_t *pkt = (const espnow_packet_t *)data;

    if (pkt->type == PKT_HELLO) {
        // Always respond to HELLO — handles both initial pairing and hub reboot/reconnect.
        // If s_hub_paired is already true, the ACK re-establishes the link on the hub side.
        int hub_ch = 0;
        if (sscanf(pkt->payload, "HELLO_FROM_HUB:ch=%d", &hub_ch) == 1 && hub_ch > 0) {
            esp_wifi_set_channel(hub_ch, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "HELLO received on ch%d — sending ACK", hub_ch);
        } else {
            ESP_LOGI(TAG, "HELLO received — sending ACK");
        }

        espnow_packet_t ack = { .type = PKT_ACK };
        snprintf(ack.payload, sizeof(ack.payload), "ACK_FROM_%s", s_own_mac_str);
        esp_now_send(s_hub_mac_bytes, (uint8_t *)&ack, sizeof(ack));

        s_hub_paired = true;
        s_scan_stop  = true;
    }
}

static void espnow_init_and_add_hub(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)GLAZIA_ESP_NOW_PMK));
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);

    // Add hub as encrypted peer — must be done BEFORE HELLO arrives
    if (!esp_now_is_peer_exist(s_hub_mac_bytes)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, s_hub_mac_bytes, 6);
        peer.channel = 0;       // follow current channel
        peer.encrypt = true;    // LMK-encrypted link
        memcpy(peer.lmk, s_lmk, 16);
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
        ESP_LOGI(TAG, "Hub added as encrypted ESP-NOW peer");
    }
}

/* ── Channel scan task ────────────────────────────────────────────────────── */
static void channel_scan_task(void *arg)
{
    uint8_t ch = 1;
    while (!s_hub_paired && !s_scan_stop) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        ch = (ch % 13) + 1;
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    vTaskDelete(NULL);
}

/* ── Event task ───────────────────────────────────────────────────────────── */
static void event_task(void *arg)
{
    int count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_hub_paired) continue;

        espnow_packet_t pkt = { .type = PKT_EVENT };
        snprintf(pkt.payload, sizeof(pkt.payload), "sensor_data_%d", count + 1);

        esp_err_t err = esp_now_send(s_hub_mac_bytes, (uint8_t *)&pkt, sizeof(pkt));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "→ %s", pkt.payload);
        } else {
            // Retry once after a short wait
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_now_send(s_hub_mac_bytes, (uint8_t *)&pkt, sizeof(pkt));
        }
        count++;
    }
}

/* ── BLE (NimBLE) ─────────────────────────────────────────────────────────── */

static int gatt_write_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);
    uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);

    if (uuid16 == 0xFF10) {
        uint16_t n = data_len < 17 ? data_len : 17;
        os_mbuf_copydata(ctxt->om, 0, n, s_ble_hub_mac);
        s_ble_hub_mac[n] = '\0';
        ESP_LOGI(TAG, "BLE: hub_mac = %s", s_ble_hub_mac);
    } else if (uuid16 == 0xFF11) {
        uint16_t n = data_len < 32 ? data_len : 32;
        os_mbuf_copydata(ctxt->om, 0, n, s_ble_prov_key);
        s_ble_prov_key[n] = '\0';
        ESP_LOGI(TAG, "BLE: provision_key received (%d chars)", n);
    }

    // Trigger when both are valid
    if (strlen(s_ble_hub_mac) >= 17 && strlen(s_ble_prov_key) == 32) {
        s_ble_got_creds  = true;
        s_ble_conn_handle = conn_handle;
        xSemaphoreGive(s_ble_done_sem);
    }

    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFF00),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = BLE_UUID16_DECLARE(0xFF10),
                .access_cb  = gatt_write_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_hub_mac_val_handle,
            },
            {
                .uuid       = BLE_UUID16_DECLARE(0xFF11),
                .access_cb  = gatt_write_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_prov_key_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void ble_on_sync(void)
{
    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)"GlaziaSensor";
    fields.name_len         = strlen("GlaziaSensor");
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, NULL, NULL);
    ESP_LOGI(TAG, "BLE advertising as 'GlaziaSensor'");
}

static void ble_host_task(void *param)
{
    nimble_port_run();  // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// Returns true if credentials were received, false on timeout
static bool ble_provision(void)
{
    s_ble_got_creds = false;
    memset(s_ble_hub_mac, 0, sizeof(s_ble_hub_mac));
    memset(s_ble_prov_key, 0, sizeof(s_ble_prov_key));
    s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_device_name_set("GlaziaSensor");
    nimble_port_freertos_init(ble_host_task);

    // Wait for write callback to give semaphore, or 60s timeout
    bool got = (xSemaphoreTake(s_ble_done_sem, pdMS_TO_TICKS(BLE_ADV_TIMEOUT_MS)) == pdTRUE);

    // Disconnect and stop advertising / BLE host
    ble_gap_adv_stop();
    if (s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    nimble_port_stop();
    vTaskDelay(pdMS_TO_TICKS(200));  // allow host task to exit cleanly

    return got && s_ble_got_creds;
}

/* ── Button ───────────────────────────────────────────────────────────────── */
static void wait_for_button(void)
{
    ESP_LOGI(TAG, "Waiting for button on GPIO%d ...", BUTTON_GPIO);
    while (gpio_get_level(BUTTON_GPIO) != 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(50));   // debounce
    while (gpio_get_level(BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Button released — starting BLE");
}

/* ── Entry point ──────────────────────────────────────────────────────────── */
void app_main(void)
{
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Own MAC
    uint8_t own_mac[6];
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);
    snprintf(s_own_mac_str, sizeof(s_own_mac_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             own_mac[0], own_mac[1], own_mac[2],
             own_mac[3], own_mac[4], own_mac[5]);

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, " Glazia Sensor  MAC: %s", s_own_mac_str);
    ESP_LOGI(TAG, " (use this MAC when pairing via the app)");
    ESP_LOGI(TAG, "===========================================");

    // Button GPIO (active-low, internal pull-up)
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    s_ble_done_sem = xSemaphoreCreateBinary();

    // ── Determine boot state ──────────────────────────────────────────────
    char hub_mac_str[18]  = {0};
    char prov_key_hex[33] = {0};

    bool has_main = nvs_load_pair(NVS_MAIN_NS, hub_mac_str, prov_key_hex);
    bool has_prov = false;
    bool is_fresh = false;

    if (!has_main) {
        has_prov = nvs_load_pair(NVS_PROV_NS, hub_mac_str, prov_key_hex);
    }
    if (!has_main && !has_prov) {
        is_fresh = true;
    }

    if (is_fresh) {
        // ── Fresh sensor: button → BLE → provisional NVS ──────────────────
        ESP_LOGI(TAG, "Fresh sensor — waiting for button + BLE provisioning");
        wait_for_button();

        bool got = ble_provision();
        if (!got) {
            ESP_LOGW(TAG, "BLE timed out — restarting");
            esp_restart();
        }

        strncpy(hub_mac_str,  s_ble_hub_mac,  sizeof(hub_mac_str)  - 1);
        strncpy(prov_key_hex, s_ble_prov_key, sizeof(prov_key_hex) - 1);
        nvs_save_pair(NVS_PROV_NS, hub_mac_str, prov_key_hex);
        has_prov = true;
    }

    // ── Convert credentials ───────────────────────────────────────────────
    mac_str_to_bytes(hub_mac_str, s_hub_mac_bytes);
    hex_to_bytes(prov_key_hex, s_lmk, 16);
    ESP_LOGI(TAG, "Hub MAC: %s  LMK: loaded", hub_mac_str);

    // ── WiFi + ESP-NOW ────────────────────────────────────────────────────
    wifi_init_for_espnow();
    espnow_init_and_add_hub();

    if (has_main) {
        // Paired — channel-scan indefinitely until hub's HELLO re-establishes link
        ESP_LOGI(TAG, "Main pairing — scanning for hub HELLO...");
        xTaskCreate(channel_scan_task, "ch_scan", 2048, NULL, 4, NULL);
        xTaskCreate(event_task, "events", 3072, NULL, 5, NULL);
        // event_task will skip sends until s_hub_paired=true (set in recv_cb)

    } else {
        // Provisional — must receive HELLO within 60s or roll back
        ESP_LOGI(TAG, "Provisional — waiting up to 60s for HELLO from hub");
        xTaskCreate(channel_scan_task, "ch_scan", 2048, NULL, 4, NULL);

        int waited = 0;
        while (!s_hub_paired && waited < HELLO_PROV_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited += 500;
        }

        s_scan_stop = true;

        if (s_hub_paired) {
            // ✓ ACK sent in recv_cb — now promote prov → main
            nvs_save_pair(NVS_MAIN_NS, hub_mac_str, prov_key_hex);
            nvs_erase_ns(NVS_PROV_NS);
            ESP_LOGI(TAG, "Pairing confirmed — promoted to main NVS");
            xTaskCreate(event_task, "events", 3072, NULL, 5, NULL);
        } else {
            // Hub never responded — roll back
            ESP_LOGE(TAG, "HELLO timeout — clearing provisional NVS and restarting");
            nvs_erase_ns(NVS_PROV_NS);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
}
