#include "button.h"
#include "state.h"
#include "ble.h"
#include "wifi.h"
#include "websocket.h"
#include "display.h"
#include "api_client.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BUTTON";

// Debounce time in ms
#define DEBOUNCE_MS 300

static void button_task(void *arg)
{
    int last_state = 1;   // pulled up, so idle = 1
    TickType_t last_press = 0;

    while (1) {
        int current = gpio_get_level(BUTTON_GPIO);

        // Detect falling edge (press)
        if (current == 0 && last_state == 1) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_press) * portTICK_PERIOD_MS > DEBOUNCE_MS) {
                last_press = now;

                ESP_LOGI(TAG, "Button pressed — current mode: %d", g_mode);

                if (g_mode == MODE_IDLE) {
                    // 1st press → start BLE pairing
                    g_mode = MODE_HUB_PAIRING;
                    display_show("HUB PAIRING", "Connect via BLE");
                    ESP_LOGI(TAG, "→ MODE_HUB_PAIRING");
                    ble_start();

                } else if (g_mode == MODE_OPERATIONAL) {
                    // 2nd press → start sensor pairing
                    g_mode = MODE_SENSOR_PAIRING;
                    display_show("SENSOR PAIRING", "Scan sensor QR");
                    ESP_LOGI(TAG, "→ MODE_SENSOR_PAIRING");
                    api_enable_sensor_pairing();
                    websocket_start();
                }
                // Any other mode — ignore the press
            }
        }

        last_state = current;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "Button initialised on GPIO%d", BUTTON_GPIO);
}
