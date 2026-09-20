#include "sensor_pairing.h"
#include "state.h"
#include "api_client.h"
#if CONFIG_ESPNOW_ENABLE
#include "espnow.h"
#else
#include "nrf_thread.h"
#include <stdio.h>
#include <stdlib.h>
#endif
#include "display.h"
#include "nvs_storage.h"
#include "hub_control_ws.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "SENSOR_PAIR";

#define SENSOR_PAIR_TIMEOUT_MS        (2 * 60 * 1000)
#define SENSOR_POLL_INTERVAL_MS       3000
#define SENSOR_POLL_STACK             6144
#if CONFIG_ESPNOW_ENABLE
#define SENSOR_PAIR_ESPNOW_MIN_WAIT_MS 15000
#endif

static TimerHandle_t  s_pair_timer          = NULL;
static TaskHandle_t   s_poll_task           = NULL;
static volatile bool  s_poll_busy           = false;
static bool           s_pair_sensor_claimed = false;

static void pairing_timeout_cb(TimerHandle_t xTimer)
{
    if (g_mode == MODE_SENSOR_PAIRING) {
        g_mode = MODE_OPERATIONAL;
    }
}

static void sensor_poll_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        hub_control_ws_stop();

        if (!api_enable_sensor_pairing()) {
            ESP_LOGW(TAG, "Sensor pairing server window failed; returning to operational mode");
            g_mode = MODE_OPERATIONAL;
            s_pair_sensor_claimed = false;
            display_show_dashboard(true);
            s_poll_busy = false;
            hub_control_ws_start();
            continue;
        }

        if (s_pair_timer) {
            xTimerReset(s_pair_timer, pdMS_TO_TICKS(100));
        }
        TickType_t s_open_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "Sensor pairing window opened — polling every %d ms for %d ms",
                 SENSOR_POLL_INTERVAL_MS, SENSOR_PAIR_TIMEOUT_MS);

        char sensor_mac[18]    = {0};
        char provision_key[33] = {0};
        char sensor_name[32]   = {0};
        char sensor_zone[32]   = {0};

        while (g_mode == MODE_SENSOR_PAIRING && !s_pair_sensor_claimed) {
            if (api_fetch_sensor_pairing(sensor_mac, provision_key,
                                         sensor_name, sizeof(sensor_name),
                                         sensor_zone, sizeof(sensor_zone))) {
                ESP_LOGI(TAG, "Pending sensor received: %s. Saving provisional NVS", sensor_mac);
                s_pair_sensor_claimed = true;
                nvs_prov_save_sensor(sensor_mac, provision_key, sensor_name, sensor_zone);

#if CONFIG_ESPNOW_ENABLE
                TickType_t elapsed = xTaskGetTickCount() - s_open_tick;
                TickType_t min_ticks = pdMS_TO_TICKS(SENSOR_PAIR_ESPNOW_MIN_WAIT_MS);
                if (elapsed < min_ticks) {
                    ESP_LOGI(TAG, "Waiting %lu ms before ESP-NOW (sensor BLE provision grace period)",
                             (unsigned long)pdTICKS_TO_MS(min_ticks - elapsed));
                    vTaskDelay(min_ticks - elapsed);
                }
                ESP_LOGI(TAG, "Starting ESP-NOW pairing for %s", sensor_mac);
                espnow_pair_sensor(sensor_mac, provision_key, sensor_name, sensor_zone);
                memset(provision_key, 0, sizeof(provision_key));
#else
                uint8_t eui64[8];
                for (int i = 0; i < 8; i++) {
                    char b[3] = { sensor_mac[i*2], sensor_mac[i*2+1], '\0' };
                    eui64[i] = (uint8_t)strtol(b, NULL, 16);
                }
                char pskd[9];
                snprintf(pskd, sizeof(pskd), "%02X%02X%02X%02X",
                         eui64[4], eui64[5], eui64[6], eui64[7]);
                ESP_LOGI(TAG, "Thread: commissioning eui64=%s pskd=%s timeout=120s", sensor_mac, pskd);
                nrf_thread_commission_sensor(eui64, pskd, 120);
#endif
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
        }

        ESP_LOGI(TAG, "Sensor pairing poll task idle, mode=%d", g_mode);
        s_poll_busy = false;
        hub_control_ws_start();
    }
}

void sensor_pairing_open_window(void)
{
    if (g_mode != MODE_OPERATIONAL && g_mode != MODE_SENSOR_PAIRING) {
        ESP_LOGW(TAG, "Cannot open sensor pairing from mode %d", g_mode);
        return;
    }
    if (s_poll_busy) {
        ESP_LOGI(TAG, "Sensor pairing start already in progress");
        return;
    }

    g_mode = MODE_SENSOR_PAIRING;
    s_pair_sensor_claimed = false;
    s_poll_busy = true;
    ESP_LOGI(TAG, "Mode transition: SENSOR_PAIRING");
    xTaskNotifyGive(s_poll_task);
}

void sensor_pairing_init(void)
{
    s_pair_timer = xTimerCreate(
        "pair_timer",
        pdMS_TO_TICKS(SENSOR_PAIR_TIMEOUT_MS),
        pdFALSE,
        NULL,
        pairing_timeout_cb
    );

    xTaskCreatePinnedToCore(sensor_poll_task, "sensor_poll", SENSOR_POLL_STACK, NULL, 5, &s_poll_task, 0);
}
