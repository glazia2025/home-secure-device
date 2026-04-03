#include "nvs_storage.h"
#include "state.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG      = "NVS";
static const char *NVS_NS   = "glazia";

void nvs_save_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(h, "wifi_ssid",   g_wifi_ssid);
    nvs_set_str(h, "wifi_pass",   g_wifi_password);
    nvs_set_str(h, "hub_mac",     g_hub_mac);
    nvs_set_str(h, "hub_secret",  g_hub_secret);
    nvs_set_str(h, "home_name",   g_home_name);

    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Credentials saved to NVS successfully");
}

bool nvs_load_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }

    size_t len;

    // Macro handles extraction and fails cleanly if a REQUIRED key is missing
    #define LOAD(key, dest, dest_size, required) \
        len = dest_size; \
        if (nvs_get_str(h, key, dest, &len) != ESP_OK) { \
            if (required) { \
                nvs_close(h); \
                return false; \
            } \
        }

    // Require SSID, Pass, and Secret. The rest are optional.
    LOAD("wifi_ssid",  g_wifi_ssid,           sizeof(g_wifi_ssid), true)
    LOAD("wifi_pass",  g_wifi_password,        sizeof(g_wifi_password), true)
    LOAD("hub_secret", g_hub_secret,           sizeof(g_hub_secret), true)
    LOAD("hub_mac",    g_hub_mac,              sizeof(g_hub_mac), false)
    LOAD("home_name",  g_home_name,            sizeof(g_home_name), false)

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
