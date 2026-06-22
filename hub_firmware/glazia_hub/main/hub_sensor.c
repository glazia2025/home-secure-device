#include "hub_sensor.h"

#include "display.h"
#include "state.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "HUB_SENSOR";

#define HUB_DHT22_GPIO              GPIO_NUM_3
#define HUB_SENSOR_TASK_STACK       2048
#define HUB_SENSOR_TASK_PRIORITY    4
#define HUB_SENSOR_SAMPLE_MS        2000
#define HUB_SENSOR_WINDOW_SIZE      3
#define HUB_SENSOR_TEMP_DELTA_C     0.1f
#define HUB_SENSOR_HUM_DELTA_PCT    1.0f
#define DHT_START_LOW_US            1200
#define DHT_START_RELEASE_US        30
#define DHT_RESPONSE_TIMEOUT_US     120
#define DHT_BIT_TIMEOUT_US          100

typedef struct {
    bool initialized;
    bool has_latest;
    bool has_displayed_valid;
    float latest_temp;
    float latest_hum;
    float last_display_temp;
    float last_display_hum;
    float temp_window[HUB_SENSOR_WINDOW_SIZE];
    float hum_window[HUB_SENSOR_WINDOW_SIZE];
    size_t window_count;
    size_t next_index;
} hub_sensor_state_t;

static hub_sensor_state_t s_state;
static TaskHandle_t s_task_handle = NULL;
static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

static bool hub_sensor_should_publish(void)
{
    switch (g_mode) {
    case MODE_REGISTERING:
    case MODE_OPERATIONAL:
    case MODE_SENSOR_PAIRING:
    case MODE_FINGERPRINT_ENROLL:
    case MODE_FINGERPRINT_VERIFY:
        return true;
    case MODE_IDLE:
    case MODE_HUB_PAIRING:
    case MODE_WIFI_CONNECTING:
    case MODE_OFFLINE:
    default:
        return false;
    }
}

static void dht_drive_line(bool level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << HUB_DHT22_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(HUB_DHT22_GPIO, level ? 1 : 0);
}

static void dht_release_line(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << HUB_DHT22_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool wait_for_level(int target_level, int timeout_us)
{
    int64_t start_us = esp_timer_get_time();
    while (gpio_get_level(HUB_DHT22_GPIO) != target_level) {
        if ((esp_timer_get_time() - start_us) > timeout_us) {
            return false;
        }
    }
    return true;
}

static bool measure_high_pulse_us(int timeout_us, int *pulse_us)
{
    int64_t start_us = esp_timer_get_time();
    while (gpio_get_level(HUB_DHT22_GPIO) == 1) {
        if ((esp_timer_get_time() - start_us) > timeout_us) {
            return false;
        }
    }

    if (pulse_us) {
        *pulse_us = (int)(esp_timer_get_time() - start_us);
    }
    return true;
}

static bool dht22_read_once(float *out_temp_c, float *out_hum_pct)
{
    uint8_t data[5] = {0};
    bool ok = false;

    taskENTER_CRITICAL(&s_dht_mux);

    dht_drive_line(false);
    ets_delay_us(DHT_START_LOW_US);
    dht_drive_line(true);
    ets_delay_us(DHT_START_RELEASE_US);
    dht_release_line();

    if (!wait_for_level(0, DHT_RESPONSE_TIMEOUT_US)) {
        goto done;
    }
    if (!wait_for_level(1, DHT_RESPONSE_TIMEOUT_US)) {
        goto done;
    }
    if (!wait_for_level(0, DHT_RESPONSE_TIMEOUT_US)) {
        goto done;
    }

    for (int bit = 0; bit < 40; bit++) {
        int pulse_us = 0;
        if (!wait_for_level(1, DHT_BIT_TIMEOUT_US)) {
            goto done;
        }
        if (!measure_high_pulse_us(DHT_BIT_TIMEOUT_US, &pulse_us)) {
            goto done;
        }

        data[bit / 8] <<= 1;
        if (pulse_us > 40) {
            data[bit / 8] |= 1;
        }
    }

    ok = true;

done:
    taskEXIT_CRITICAL(&s_dht_mux);
    dht_release_line();

    if (!ok) {
        return false;
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return false;
    }

    uint16_t raw_hum = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_temp = ((uint16_t)(data[2] & 0x7F) << 8) | data[3];

    float humidity = raw_hum * 0.1f;
    float temperature = raw_temp * 0.1f;
    if (data[2] & 0x80) {
        temperature = -temperature;
    }

    if (humidity < 0.0f || humidity > 100.0f || temperature < -40.0f || temperature > 80.0f) {
        return false;
    }

    if (out_temp_c) {
        *out_temp_c = temperature;
    }
    if (out_hum_pct) {
        *out_hum_pct = humidity;
    }
    return true;
}

static float average_window(const float *values, size_t count)
{
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
    }
    return count > 0 ? sum / (float)count : 0.0f;
}

static float trimmed_mean_3(const float *values)
{
    float ordered[HUB_SENSOR_WINDOW_SIZE];
    memcpy(ordered, values, sizeof(ordered));

    for (size_t i = 0; i < HUB_SENSOR_WINDOW_SIZE; i++) {
        for (size_t j = i + 1; j < HUB_SENSOR_WINDOW_SIZE; j++) {
            if (ordered[j] < ordered[i]) {
                float tmp = ordered[i];
                ordered[i] = ordered[j];
                ordered[j] = tmp;
            }
        }
    }

    return ordered[1];
}

static void push_window_sample(float temp, float hum)
{
    s_state.temp_window[s_state.next_index] = temp;
    s_state.hum_window[s_state.next_index] = hum;
    s_state.next_index = (s_state.next_index + 1U) % HUB_SENSOR_WINDOW_SIZE;
    if (s_state.window_count < HUB_SENSOR_WINDOW_SIZE) {
        s_state.window_count++;
    }
}

static void filter_latest_sample(float *out_temp, float *out_hum)
{
    if (s_state.window_count >= HUB_SENSOR_WINDOW_SIZE) {
        *out_temp = trimmed_mean_3(s_state.temp_window);
        *out_hum = trimmed_mean_3(s_state.hum_window);
        return;
    }

    *out_temp = average_window(s_state.temp_window, s_state.window_count);
    *out_hum = average_window(s_state.hum_window, s_state.window_count);
}

static bool display_change_is_significant(float temp, float hum)
{
    if (!s_state.has_displayed_valid) {
        return true;
    }

    return fabsf(temp - s_state.last_display_temp) >= HUB_SENSOR_TEMP_DELTA_C ||
           fabsf(hum - s_state.last_display_hum) >= HUB_SENSOR_HUM_DELTA_PCT;
}

static void hub_sensor_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t consecutive_failures = 0;

    display_update_temp_hum(0.0f, 0.0f);

    while (1) {
        if (!hub_sensor_should_publish()) {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HUB_SENSOR_SAMPLE_MS));
            continue;
        }

        float raw_temp = 0.0f;
        float raw_hum = 0.0f;
        bool read_ok = dht22_read_once(&raw_temp, &raw_hum);
        if (!read_ok) {
            vTaskDelay(pdMS_TO_TICKS(20));
            read_ok = dht22_read_once(&raw_temp, &raw_hum);
        }

        if (read_ok) {
            consecutive_failures = 0;
            push_window_sample(raw_temp, raw_hum);

            float filtered_temp = 0.0f;
            float filtered_hum = 0.0f;
            filter_latest_sample(&filtered_temp, &filtered_hum);

            s_state.has_latest = true;
            s_state.latest_temp = filtered_temp;
            s_state.latest_hum = filtered_hum;

            if (display_change_is_significant(filtered_temp, filtered_hum)) {
                display_update_temp_hum(filtered_temp, filtered_hum);
                s_state.has_displayed_valid = true;
                s_state.last_display_temp = filtered_temp;
                s_state.last_display_hum = filtered_hum;
            }
        } else {
            consecutive_failures++;
            if (consecutive_failures == 1 || (consecutive_failures % 10U) == 0U) {
                ESP_LOGW(TAG, "DHT22 read failed on GPIO%d (%lu consecutive failures)",
                         HUB_DHT22_GPIO, (unsigned long)consecutive_failures);
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HUB_SENSOR_SAMPLE_MS));
    }
}

esp_err_t hub_sensor_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    dht_release_line();

    if (xTaskCreatePinnedToCore(hub_sensor_task,
                                "hub_sensor",
                                HUB_SENSOR_TASK_STACK,
                                NULL,
                                HUB_SENSOR_TASK_PRIORITY,
                                &s_task_handle, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create hub sensor task");
        return ESP_FAIL;
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "Hub DHT22 sensor task started on GPIO%d", HUB_DHT22_GPIO);
    return ESP_OK;
}

bool hub_sensor_get_latest(float *out_temp_c, float *out_hum_pct)
{
    if (!s_state.has_latest) {
        return false;
    }

    if (out_temp_c) {
        *out_temp_c = s_state.latest_temp;
    }
    if (out_hum_pct) {
        *out_hum_pct = s_state.latest_hum;
    }
    return true;
}
