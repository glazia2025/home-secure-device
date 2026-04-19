#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define TAG        "VIB_TEST"
#define PIN_DO     GPIO_NUM_4
#define ADC_CHAN   ADC_CHANNEL_3   // GPIO3 = ADC1_CH3 on ESP32-C3

void app_main(void)
{
    // --- Digital pin setup ---
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_DO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);

    // --- ADC setup ---
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_new_unit(&unit_cfg, &adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_11,   // 0–3.3V range
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc_handle, ADC_CHAN, &chan_cfg);

    ESP_LOGI(TAG, "SW-18010P vibration sensor test started");
    ESP_LOGI(TAG, "Tap or shake the sensor...");

    while (1) {
        int do_val  = gpio_get_level(PIN_DO);
        int ao_raw  = 0;
        adc_oneshot_read(adc_handle, ADC_CHAN, &ao_raw);

        // DO is active LOW — LOW means vibration detected
        bool vibrating = (do_val == 0);

        ESP_LOGI(TAG, "DO: %-10s | AO raw: %4d%s",
            vibrating ? "VIBRATING" : "still",
            ao_raw,
            vibrating ? "  <-- tap!" : "");

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
