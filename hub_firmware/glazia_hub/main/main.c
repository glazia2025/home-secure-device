#include "nvs_flash.h"
#include "nvs_storage.h"
#include "wifi.h"
#include "button.h"
#include "display.h"
#include "state.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

// Instantiate globals
hub_mode_t g_mode = MODE_IDLE;
char g_wifi_ssid[64] = {0}, g_wifi_password[64] = {0}, g_provisioning_token[64] = {0};
char g_hub_mac[18] = {0}, g_hub_secret[128] = {0}, g_home_id[64] = {0}, g_home_name[64] = {0};

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_hub_mac, sizeof(g_hub_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "HUB MAC: %s", g_hub_mac);

    display_init();
    button_init();

    // AUTO-CONNECT LOGIC
    if (nvs_load_credentials()) {
        ESP_LOGI(TAG, "Saved credentials found! Reconnecting...");
        display_show("Auto-Connect", g_wifi_ssid);
        g_mode = MODE_WIFI_CONNECTING;
        wifi_connect(g_wifi_ssid, g_wifi_password);
    } else {
        display_show("Glazia Hub", "Press button");
    }

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
