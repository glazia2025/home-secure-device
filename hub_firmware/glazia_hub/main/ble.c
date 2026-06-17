#include "ble.h"
#include "state.h"
#include "wifi.h"
#include "display.h"

#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "BLE";

#define SERVICE_UUID    0x00FF
#define CHAR_UUID_SSID  0xFF01
#define CHAR_UUID_PASS  0xFF02
#define CHAR_UUID_TOKEN 0xFF03

static bool got_ssid     = false;
static bool got_password = false;
static bool got_token    = false;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

static void log_token_summary(const char *label, const char *token)
{
    size_t len = token ? strlen(token) : 0;
    if (len >= 4) {
        ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] %s received: len=%u suffix=%.4s", label, (unsigned)len, token + len - 4);
    } else {
        ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] %s received: len=%u", label, (unsigned)len);
    }
}

// ── NEW: Transition Task ──────────────────────────────────────────────────
static void transition_to_wifi_task(void *arg)
{
    ESP_LOGI(TAG, "Credential handoff task started; stopping BLE before WiFi");
    // Wait 500ms to allow the final BLE ACK to reach the phone/nRF app
    vTaskDelay(pdMS_TO_TICKS(500));

    // Safely stop BLE from outside the NimBLE thread
    ble_stop();

    g_mode = MODE_WIFI_CONNECTING;
    ESP_LOGI(TAG, "Mode transition: WIFI_CONNECTING");
    wifi_connect(g_wifi_ssid, g_wifi_password);

    // Delete this temporary task once we hand off to WiFi
    vTaskDelete(NULL);
}

// ── UPDATED: Check and Proceed ────────────────────────────────────────────
static void check_and_proceed(void)
{
    if (got_ssid && got_password && got_token) {
        ESP_LOGI(TAG, "All BLE credentials received for SSID '%s'; starting WiFi transition",
                 g_wifi_ssid);

        // Reset the token flag so this doesn't accidentally trigger twice
        got_token = false;

        // Spawn the transition task to avoid deadlocking the NimBLE thread
        if (xTaskCreate(transition_to_wifi_task, "wifi_trans", 12288, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create WiFi transition task");
        }
    }
}

// ── GATT Write Callback ───────────────────────────────────────────────────
static int gatt_write_cb(uint16_t conn_h, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);
    uint16_t len    = OS_MBUF_PKTLEN(ctxt->om);
    char buf[128]   = {0};
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    buf[len] = '\0';

    if (uuid16 == CHAR_UUID_SSID) {
        strncpy(g_wifi_ssid, buf, sizeof(g_wifi_ssid) - 1);
        got_ssid = true;
        ESP_LOGI(TAG, "BLE credential received: SSID='%s'", g_wifi_ssid);
    } else if (uuid16 == CHAR_UUID_PASS) {
        strncpy(g_wifi_password, buf, sizeof(g_wifi_password) - 1);
        got_password = true;
        ESP_LOGI(TAG, "BLE credential received: password len=%u", (unsigned)strlen(g_wifi_password));
    } else if (uuid16 == CHAR_UUID_TOKEN) {
        strncpy(g_provisioning_token, buf, sizeof(g_provisioning_token) - 1);
        got_token = true;
        log_token_summary("BLE provisioning token", g_provisioning_token);
    } else {
        ESP_LOGW(TAG, "Write to unknown BLE characteristic 0x%04X len=%u", uuid16, len);
    }

    check_and_proceed();
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = BLE_UUID16_DECLARE(CHAR_UUID_SSID),
                .access_cb = gatt_write_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid      = BLE_UUID16_DECLARE(CHAR_UUID_PASS),
                .access_cb = gatt_write_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid      = BLE_UUID16_DECLARE(CHAR_UUID_TOKEN),
                .access_cb = gatt_write_cb,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 }
        },
    },
    { 0 }
};

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE phone connected, handle=%u", conn_handle);
            display_show("BLE Connected", "Send credentials");
        } else {
            ESP_LOGW(TAG, "BLE connection failed status=%d; restarting advertising", event->connect.status);
            ble_start();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected; have ssid=%d pass=%d token=%d",
                 got_ssid, got_password, got_token);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (!got_ssid || !got_password || !got_token) {
            ESP_LOGI(TAG, "BLE credentials incomplete; restarting advertising");
            ble_start();
        }
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct ble_hs_adv_fields fields = {0};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len         = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE advertising field setup failed rc=%d", rc);
        return;
    }
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE advertising started as '%s'", BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "BLE advertising start failed rc=%d", rc);
    }
}

static void ble_on_sync(void)
{
    uint8_t addr_type;
    int rc = ble_hs_id_infer_auto(0, &addr_type);
    ESP_LOGI(TAG, "BLE host synced, addr_type=%u rc=%d", addr_type, rc);
    start_advertising();
}

static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    ESP_LOGI(TAG, "NimBLE host task stopped");
    nimble_port_freertos_deinit();
}

void ble_start(void)
{
    ESP_LOGI(TAG, "Starting NimBLE...");
    nimble_port_init();
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(nimble_host_task);
    display_show("HUB PAIRING", "Connect via BLE");
}

void ble_stop(void)
{
    ESP_LOGI(TAG, "Stopping BLE");
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    nimble_port_stop();
    nimble_port_deinit();
    ESP_LOGI(TAG, "BLE stopped");
}
