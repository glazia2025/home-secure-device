#include "wifi.h"
#include "state.h"
#include "api_client.h"
#include "camera_stream.h"
#include "display.h"
#include "hub_control_ws.h"
#include "hub_sensor.h"
#include "aqi_sensor.h"
#include "espnow.h"
#include "nvs_storage.h"
#include "fingerprint.h"

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
static bool s_offline_mode    = false;
static bool s_platform_ready  = false;
#define WIFI_MAX_RETRIES 10

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started; connecting to AP");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_offline_mode) {
            ESP_LOGI(TAG, "WiFi disconnected for offline mode");
            return;
        }

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
            char prov_mac[18] = {0}, prov_key[33] = {0}, prov_name[32] = {0}, prov_zone[32] = {0};
            if (nvs_prov_load_sensor(prov_mac, prov_key,
                                     prov_name, sizeof(prov_name),
                                     prov_zone, sizeof(prov_zone))) {
                ESP_LOGI(TAG, "WiFi back — resuming provisional sensor pairing for %s", prov_mac);
                espnow_pair_sensor(prov_mac, prov_key, prov_name, prov_zone);
            }
        }
    }
}

esp_err_t wifi_platform_init(void)
{
    if (s_platform_ready) return ESP_OK;

    ESP_LOGI(TAG, "WiFi platform step: esp_netif_init");
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WiFi platform step done: esp_netif_init (%s)", esp_err_to_name(err));

    ESP_LOGI(TAG, "WiFi platform step: esp_event_loop_create_default");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WiFi platform step done: esp_event_loop_create_default (%s)", esp_err_to_name(err));

    s_platform_ready = true;
    return ESP_OK;
}

void wifi_connect(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Preparing WiFi connection to SSID '%s'", ssid ? ssid : "");

    ESP_LOGI(TAG, "WiFi step: create event group");
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: create event group");

    ESP_LOGI(TAG, "WiFi step: platform init");
    esp_err_t err = wifi_platform_init();
    if (err != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: platform init");

    ESP_LOGI(TAG, "WiFi step: esp_netif_create_default_wifi_sta");
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: esp_netif_create_default_wifi_sta");

    ESP_LOGI(TAG, "WiFi step: esp_wifi_init");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: esp_wifi_init");

    ESP_LOGI(TAG, "WiFi step: register WIFI_EVENT handler");
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_EVENT handler register failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: register WIFI_EVENT handler");

    ESP_LOGI(TAG, "WiFi step: register IP_EVENT handler");
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP_EVENT handler register failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: register IP_EVENT handler");

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,     ssid ? ssid : "",         sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, password ? password : "", sizeof(wifi_cfg.sta.password) - 1);

    if (password && strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_LOGI(TAG, "WiFi step: esp_wifi_set_mode");
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: esp_wifi_set_mode");

    ESP_LOGI(TAG, "WiFi step: esp_wifi_set_config");
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: esp_wifi_set_config; password len=%u",
             (unsigned)strlen(password ? password : ""));

    ESP_LOGI(TAG, "WiFi step: pre-start delay");
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "WiFi step done: pre-start delay");

    ESP_LOGI(TAG, "WiFi step: esp_wifi_start");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WiFi step done: esp_wifi_start");

    ESP_LOGI(TAG, "WiFi step: esp_wifi_set_max_tx_power");
    err = esp_wifi_set_max_tx_power(56);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi step done: esp_wifi_set_max_tx_power");
    }

    ESP_LOGI(TAG, "WiFi step: wait for connect/fail");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi step done: wait bits=0x%lx", (unsigned long)bits);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP!");
        s_initial_connect = false;

        esp_err_t hub_sensor_err = hub_sensor_init();
        if (hub_sensor_err != ESP_OK) {
            ESP_LOGW(TAG, "Hub sensor delayed init failed: %s", esp_err_to_name(hub_sensor_err));
        }

        esp_err_t aqi_sensor_err = aqi_sensor_init();
        if (aqi_sensor_err != ESP_OK) {
            ESP_LOGW(TAG, "AQI sensor delayed init failed: %s", esp_err_to_name(aqi_sensor_err));
        }

        ESP_LOGI(TAG, "Initializing ESP-NOW after WiFi connect");
        espnow_init();

        if (strlen(g_hub_secret) > 0) {
            // Already registered — go operational and restore saved sensors
            g_mode = MODE_OPERATIONAL;
            display_user_name(g_user_name);
            display_show_dashboard(true);
            display_hub_location(g_home_name);
            ESP_LOGI(TAG, "Already registered. Mode transition: OPERATIONAL, home='%s'", g_home_name);
            fp_start_enroll_if_needed();
            hub_control_ws_start();
            espnow_reconnect_saved_sensors();

            // Also check if a sensor pairing was interrupted (provisional NVS)
            char prov_mac[18] = {0}, prov_key[33] = {0}, prov_name[32] = {0}, prov_zone[32] = {0};
            if (nvs_prov_load_sensor(prov_mac, prov_key,
                                     prov_name, sizeof(prov_name),
                                     prov_zone, sizeof(prov_zone))) {
                ESP_LOGI(TAG, "Found interrupted sensor pairing — resuming for %s", prov_mac);
                espnow_pair_sensor(prov_mac, prov_key, prov_name, prov_zone);
            }
        } else {
            // First boot — register with server
            ESP_LOGI(TAG, "No hub secret present; registering hub with server");
            api_register_hub();
        }

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid ? ssid : "");
        display_show("WiFi failed", "Check credentials/AP");
    }
}

void wifi_enter_offline_mode(void)
{
    ESP_LOGI(TAG, "Entering hub offline mode");
    s_offline_mode = true;
    espnow_deinit();
    hub_control_ws_stop();
    camera_stream_stop();

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
    g_mode = MODE_OFFLINE;
    display_show_dashboard(false);
}

bool wifi_resume_from_offline_mode(void)
{
    ESP_LOGI(TAG, "Resuming hub from offline mode");
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "WiFi event group missing; cannot resume");
        return false;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_offline_mode = false;

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        s_offline_mode = true;
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi resume timed out");
        s_offline_mode = true;
        return false;
    }

    espnow_init();
    espnow_reconnect_saved_sensors();
    g_mode = MODE_OPERATIONAL;
    ESP_LOGI(TAG, "Mode transition: OPERATIONAL after WiFi resume");
    display_user_name(g_user_name);
    display_show_dashboard(true);
    display_hub_location(g_home_name);
    hub_control_ws_start();
    return true;
}
