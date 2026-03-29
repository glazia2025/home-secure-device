#include "wifi.h"
#include "state.h"
#include "display.h"
#include "espnow.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
#define WIFI_MAX_RETRIES 10

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRIES) {
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected — retry %d/%d", s_retry_num, WIFI_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000)); // 1-second backoff before retrying
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Preparing WiFi connection...");
    display_show("WiFi", "Connecting...");

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    // Only enforce WPA2 if password is provided
    if (strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    // ─── THE CRITICAL POWER FIX ──────────────────────────────────────────
    // 1. Give the hardware's voltage regulator time to stabilize
    //    after the massive current swing of shutting down BLE.
    ESP_LOGI(TAG, "Resting voltage regulator before WiFi start...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    // 2. Start the radio
    esp_wifi_start();

    // 3. Immediately apply the proven TX power limit from your old project
    //    (56 * 0.25dBm = 14dBm) to prevent USB brownouts during AP negotiation.
    esp_wifi_set_max_tx_power(56);
    // ─────────────────────────────────────────────────────────────────────

    // Wait for connection result
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP successfully!");
        display_show("WiFi Connected", "Registering...");
        g_mode = MODE_OPERATIONAL; // Or whatever your state logic requires

        // Init ESP-NOW now that the WiFi hardware is active and stable
        espnow_init();

        // -------------------------------------------------------------
        // TODO: Call your backend API registration function here!
        // api_client_register_hub();
        // -------------------------------------------------------------

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        display_show("WiFi Error", "Check credentials");

        // Wait a moment so the user can read the error, then reboot safely
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    vEventGroupDelete(s_wifi_event_group);
}
