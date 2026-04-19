#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define REED_PIN GPIO_NUM_10
#define TAG "REED"

void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << REED_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Reed switch test started on GPIO %d", REED_PIN);
    ESP_LOGI(TAG, "Waiting for switch state changes...");

    int last_state = -1;

    while (1) {
        int state = gpio_get_level(REED_PIN);

        if (state != last_state) {
            if (state == 0) {
                ESP_LOGI(TAG, "CLOSED - magnet detected (or pins shorted)");
            } else {
                ESP_LOGI(TAG, "OPEN   - no magnet");
            }
            last_state = state;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
