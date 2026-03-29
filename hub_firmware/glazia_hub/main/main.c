#include "state.h"
#include "button.h"
#include "display.h"
#include "espnow.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"       // <--- Added for MAC address reading
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

// ── Global state definitions ──────────────────────────────────────────────
hub_mode_t g_mode = MODE_IDLE;

char g_wifi_ssid[64]           = {0};
char g_wifi_password[64]       = {0};
char g_provisioning_token[64]  = {0};

char g_hub_mac[18]             = {0};
char g_hub_secret[128]         = {0};
char g_home_id[64]             = {0};
char g_home_name[64]           = {0};

// ── Entry point ───────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "Glazia Hub booting...");

    // Init NVS (required for WiFi and MAC reading)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // --- PRINT MAC ADDRESSES ---
    uint8_t mac_wifi[6];
    uint8_t mac_bt[6];
    esp_read_mac(mac_wifi, ESP_MAC_WIFI_STA);
    esp_read_mac(mac_bt, ESP_MAC_BT);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WIFI MAC ADDRESS: %02X:%02X:%02X:%02X:%02X:%02X  <-- USE THIS IN SCRIPT",
             mac_wifi[0], mac_wifi[1], mac_wifi[2], mac_wifi[3], mac_wifi[4], mac_wifi[5]);
    ESP_LOGI(TAG, "BLUETOOTH MAC:    %02X:%02X:%02X:%02X:%02X:%02X",
             mac_bt[0], mac_bt[1], mac_bt[2], mac_bt[3], mac_bt[4], mac_bt[5]);
    ESP_LOGI(TAG, "========================================");
    // ---------------------------

    // Init display first so we can show boot screen
    display_init();
    display_show("Glazia Hub", "Press button");

    // Init ESP-NOW (WiFi must be init'd first — done inside wifi_connect)
    // We init ESP-NOW after WiFi connects, not here.
    // espnow_init() is called from wifi.c after connection succeeds.

    // Init button — this starts the state machine
    button_init();

    ESP_LOGI(TAG, "Ready — waiting for button press");

    // Main loop — just keeps the app alive
    // All logic is driven by button presses and callbacks
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
