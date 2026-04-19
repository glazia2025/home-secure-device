#include "wifi.h"
#include "state.h"
#include "api_client.h"
#include "display.h"
#include "espnow.h"
#include "nvs_storage.h"

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

static int  s_retry_num       = 0;
static bool s_initial_connect = true;
#define WIFI_MAX_RETRIES 10

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_num++;
        ESP_LOGW(TAG, "WiFi dropped — retry %d", s_retry_num);

        if (s_initial_connect && s_retry_num >= WIFI_MAX_RETRIES) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_wifi_connect();
            if (!s_initial_connect) {
                display_show("WiFi dropped", "Reconnecting...");
            }
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (!s_initial_connect) {
            // WiFi recovered mid-operation.
            // If there is an unfinished sensor pairing in provisional NVS, retry it.
            char prov_mac[18] = {0}, prov_key[33] = {0};
            if (nvs_prov_load_sensor(prov_mac, prov_key)) {
                ESP_LOGI(TAG, "WiFi back — resuming provisional sensor pairing for %s", prov_mac);
                display_show("WiFi back", "Resuming pair");
                espnow_pair_sensor(prov_mac, prov_key);
            } else {
                display_show("WiFi back", "Reconnected");
            }
        }
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
    strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    if (strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);

    // Brief delay: lets voltage regulator recover from BLE shutdown
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_wifi_start();
    esp_wifi_set_max_tx_power(56);   // limit peak current on USB

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP!");
        s_initial_connect = false;

        espnow_init();

        if (strlen(g_hub_secret) > 0) {
            // Already registered — go operational and restore saved sensors
            g_mode = MODE_OPERATIONAL;
            if (strlen(g_user_name) > 0) {
                display_show(g_home_name, g_user_name);
            } else {
                display_show("Hub Ready!", g_home_name);
            }
            ESP_LOGI(TAG, "Already registered, going operational");
            espnow_reconnect_saved_sensors();

            // Also check if a sensor pairing was interrupted (provisional NVS)
            char prov_mac[18] = {0}, prov_key[33] = {0};
            if (nvs_prov_load_sensor(prov_mac, prov_key)) {
                ESP_LOGI(TAG, "Found interrupted sensor pairing — resuming for %s", prov_mac);
                display_show("Resuming pair", prov_mac);
                espnow_pair_sensor(prov_mac, prov_key);
            }
        } else {
            // First boot — register with server
            display_show("WiFi Connected", "Registering...");
            api_register_hub();
        }

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        display_show("WiFi Error", "Check credentials");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
}
