#include "aqi_sensor.h"

#include "display.h"
#include "state.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AQI_SENSOR";

#define MQ135_ADC_UNIT          ADC_UNIT_2
#define MQ135_ADC_CHANNEL       ADC_CHANNEL_9
#define AQI_TASK_STACK          2048
#define AQI_TASK_PRIORITY       4
#define AQI_WARMUP_MS           10000
#define AQI_SAMPLE_COUNT        20
#define AQI_SAMPLE_DELAY_MS     20
#define AQI_UPDATE_DELAY_MS     1000

static adc_oneshot_unit_handle_t s_adc_handle;
static TaskHandle_t s_task_handle;

static bool aqi_sensor_should_publish(void)
{
    switch (g_mode) {
    case MODE_REGISTERING:
    case MODE_OPERATIONAL:
    case MODE_SENSOR_PAIRING:
    case MODE_FINGERPRINT_ENROLL:
    case MODE_FINGERPRINT_VERIFY:
        return true;
    default:
        return false;
    }
}

static esp_err_t read_sensor_avg(int *out_raw)
{
    int sum = 0;

    for (int i = 0; i < AQI_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc_handle, MQ135_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            return err;
        }
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(AQI_SAMPLE_DELAY_MS));
    }

    *out_raw = sum / AQI_SAMPLE_COUNT;
    return ESP_OK;
}

static const char *aqi_classifier(float aqi)
{
    if (aqi < 51.0f) return "good";
    if (aqi < 101.0f) return "nominal";
    if (aqi < 201.0f) return "moderate";
    if (aqi < 301.0f) return "poor";
    if (aqi < 401.0f) return "very_poor";
    return "severe";
}

static void aqi_sensor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(AQI_WARMUP_MS));

    while (true) {
        int raw = 0;
        esp_err_t err = read_sensor_avg(&raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MQ-135 ADC read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(AQI_UPDATE_DELAY_MS));
            continue;
        }

        float aqi = (float)(raw - 600) / 5.0f;
        if (aqi < 0.0f) aqi = 0.0f;
        if (aqi > 500.0f) aqi = 500.0f;

        if (aqi_sensor_should_publish()) {
            display_update_aqi(aqi, aqi_classifier(aqi));
        }

        vTaskDelay(pdMS_TO_TICKS(AQI_UPDATE_DELAY_MS));
    }
}

esp_err_t aqi_sensor_init(void)
{
    if (s_task_handle) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = MQ135_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQ-135 ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc_handle, MQ135_ADC_CHANNEL, &channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQ-135 ADC channel config failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    if (xTaskCreatePinnedToCore(aqi_sensor_task, "aqi_sensor", AQI_TASK_STACK, NULL,
                                AQI_TASK_PRIORITY, &s_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQ-135 task");
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
