#include "nvs_storage.h"
#include "state.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG       = "NVS";
static const char *NVS_NS    = "glazia";       // main namespace
static const char *NVS_PROV  = "glazia_prov";  // provisional namespace

// ── Main namespace ─────────────────────────────────────────────────────────

void nvs_save_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(h, "wifi_ssid",  g_wifi_ssid);
    nvs_set_str(h, "wifi_pass",  g_wifi_password);
    nvs_set_str(h, "hub_mac",    g_hub_mac);
    nvs_set_str(h, "hub_secret", g_hub_secret);
    nvs_set_str(h, "home_name",  g_home_name);
    nvs_set_str(h, "user_name",  g_user_name);

    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Credentials saved to NVS");
}

bool nvs_load_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }

    size_t len;

    #define LOAD(key, dest, dest_size, required) \
        len = dest_size; \
        if (nvs_get_str(h, key, dest, &len) != ESP_OK) { \
            if (required) { nvs_close(h); return false; } \
        }

    LOAD("wifi_ssid",  g_wifi_ssid,    sizeof(g_wifi_ssid),    true)
    LOAD("wifi_pass",  g_wifi_password, sizeof(g_wifi_password), true)
    LOAD("hub_secret", g_hub_secret,   sizeof(g_hub_secret),   true)
    LOAD("hub_mac",    g_hub_mac,      sizeof(g_hub_mac),      false)
    LOAD("home_name",  g_home_name,    sizeof(g_home_name),    false)
    LOAD("user_name",  g_user_name,    sizeof(g_user_name),    false)

    #undef LOAD

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded credentials from NVS");
    return true;
}

void nvs_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS credentials cleared");
    }
}

void nvs_save_sensors(const char macs[][18], const uint8_t keys[][16], int count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "sensor_count", (uint8_t)count);
    for (int i = 0; i < count; i++) {
        char mac_key[24], lmk_key[24];
        snprintf(mac_key, sizeof(mac_key), "sensor_%d", i);
        snprintf(lmk_key, sizeof(lmk_key), "sensor_key_%d", i);

        nvs_set_str(h, mac_key, macs[i]);
        nvs_set_blob(h, lmk_key, keys[i], 16);
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d sensor(s) with LMK keys to NVS", count);
}

int nvs_load_sensors(char macs[][18], uint8_t keys[][16], int max_count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t count = 0;
    nvs_get_u8(h, "sensor_count", &count);
    if (count > max_count) count = max_count;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        char mac_key[24], lmk_key[24];
        snprintf(mac_key, sizeof(mac_key), "sensor_%d", i);
        snprintf(lmk_key, sizeof(lmk_key), "sensor_key_%d", i);

        size_t mac_len = 18;
        if (nvs_get_str(h, mac_key, macs[loaded], &mac_len) != ESP_OK) continue;

        if (keys != NULL) {
            size_t lmk_len = 16;
            if (nvs_get_blob(h, lmk_key, keys[loaded], &lmk_len) != ESP_OK) {
                // Missing LMK — skip this sensor so we don't reconnect unencrypted
                ESP_LOGW(TAG, "No LMK for sensor %d (%s) — skipping", i, macs[loaded]);
                continue;
            }
        }

        loaded++;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d sensor(s) from NVS", loaded);
    return loaded;
}

// ── Provisional namespace ──────────────────────────────────────────────────

void nvs_prov_save_sensor(const char *sensor_mac, const char *provision_key_hex)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PROV, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open provisional NVS");
        return;
    }
    nvs_set_str(h, "prov_mac", sensor_mac);
    nvs_set_str(h, "prov_key", provision_key_hex);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Provisional sensor saved: %s", sensor_mac);
}

bool nvs_prov_load_sensor(char *out_sensor_mac, char *out_provision_key_hex)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PROV, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = 18;
    bool ok = (nvs_get_str(h, "prov_mac", out_sensor_mac, &len) == ESP_OK);
    if (ok) {
        len = 33;
        ok = (nvs_get_str(h, "prov_key", out_provision_key_hex, &len) == ESP_OK);
    }

    nvs_close(h);
    return ok;
}

void nvs_prov_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PROV, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Provisional NVS cleared");
    }
}
