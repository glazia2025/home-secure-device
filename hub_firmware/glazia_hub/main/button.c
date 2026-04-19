#include "button.h"
#include "state.h"
#include "api_client.h"
#include "espnow.h"
#include "ble.h"
#include "display.h"
#include "nvs_storage.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "BUTTON";

// ── Sensor pairing: 2-minute polling window ───────────────────────────────
#define SENSOR_PAIR_TIMEOUT_MS   (2 * 60 * 1000)   // 2 minutes total
#define SENSOR_POLL_INTERVAL_MS  3000               // check server every 3 s

static TimerHandle_t s_pair_timer = NULL;
static TaskHandle_t  s_poll_task  = NULL;

static void pairing_timeout_cb(TimerHandle_t xTimer)
{
    // NOTE: do not call display_show or ESP_LOGI here — display_show calls ESP_LOGI
    // internally, and ESP_LOGI's printf chain overflows the Tmr Svc task stack (~2KB).
    // Poll task detects g_mode change and exits on its own.
    if (g_mode == MODE_SENSOR_PAIRING) {
        g_mode = MODE_OPERATIONAL;
    }
}

// Polls GET /api/device/hubs/pending-sensor every 3 s while the 2-minute window
// is open. Each time a sensor is returned it immediately kicks off ESP-NOW pairing
// and keeps polling for more — supports batch pairing of multiple sensors.
static void sensor_poll_task(void *arg)
{
    char sensor_mac[18]    = {0};
    char provision_key[33] = {0};

    while (g_mode == MODE_SENSOR_PAIRING) {
        if (api_fetch_sensor_pairing(sensor_mac, provision_key)) {
            ESP_LOGI(TAG, "Got sensor from server: %s — starting ESP-NOW pair", sensor_mac);
            nvs_prov_save_sensor(sensor_mac, provision_key);
            espnow_pair_sensor(sensor_mac, provision_key);

            // Don't break — keep polling immediately for more pending sensors
            // (batch: user may have registered multiple sensors before pressing button)
            memset(sensor_mac, 0, sizeof(sensor_mac));
            memset(provision_key, 0, sizeof(provision_key));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Sensor pairing poll task exiting");
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

// ── Button task ───────────────────────────────────────────────────────────
void button_task(void *arg)
{
    ESP_LOGI(TAG, "Button task running on GPIO %d", BUTTON_GPIO);
    while (1) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));   // debounce
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Button pressed! Mode: %d", g_mode);

                if (g_mode == MODE_IDLE) {
                    // 1st press: start hub BLE pairing
                    g_mode = MODE_HUB_PAIRING;
                    display_show("HUB PAIRING", "Connect via BLE");
                    ble_start();

                } else if (g_mode == MODE_OPERATIONAL) {
                    // 2nd press: open pairing window on server, start polling
                    g_mode = MODE_SENSOR_PAIRING;
                    display_show("SENSOR PAIRING", "Waiting for app");
                    api_enable_sensor_pairing();

                    // Start or reset the 2-minute timeout
                    if (s_pair_timer) {
                        xTimerReset(s_pair_timer, pdMS_TO_TICKS(100));
                    }

                    // Spawn polling task (only one at a time)
                    if (s_poll_task == NULL) {
                        xTaskCreate(sensor_poll_task, "sensor_poll", 4096,
                                    NULL, 5, &s_poll_task);
                    }

                    ESP_LOGI(TAG, "Sensor pairing window opened (2 min)");
                }

                // Wait for release
                while (gpio_get_level(BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
    };
    gpio_config(&io_conf);

    s_pair_timer = xTimerCreate(
        "pair_timer",
        pdMS_TO_TICKS(SENSOR_PAIR_TIMEOUT_MS),
        pdFALSE,    // one-shot
        NULL,
        pairing_timeout_cb
    );

    xTaskCreate(button_task, "button_task", 5120, NULL, 10, NULL);
}
