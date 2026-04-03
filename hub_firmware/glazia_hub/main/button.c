#include "button.h"
#include "state.h"
#include "api_client.h"
#include "ble.h"
#include "display.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON";

void button_task(void *arg) {
    ESP_LOGI(TAG, "Button task running on GPIO %d", BUTTON_GPIO);
    while (1) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Button pressed! Current Mode: %d", g_mode);
                if (g_mode == MODE_IDLE) {
                    g_mode = MODE_HUB_PAIRING;
                    display_show("HUB PAIRING", "Connect via BLE");
                    ble_start();
                } else if (g_mode == MODE_OPERATIONAL) {
                    g_mode = MODE_SENSOR_PAIRING;
                    display_show("SENSOR PAIRING", "Scan sensor QR");
                    api_enable_sensor_pairing();
                }
                while(gpio_get_level(BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void button_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
    xTaskCreate(button_task, "button_task", 5120, NULL, 10, NULL);
}
