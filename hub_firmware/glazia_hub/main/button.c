#include "button.h"
#include "state.h"
#include "api_client.h"
#include "espnow.h"
#include "ble.h"
#include "display.h"
#include "fingerprint.h"
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
#define SENSOR_PAIR_START_STACK  4096
#define SENSOR_POLL_STACK        4096

static TimerHandle_t s_pair_timer = NULL;
static TaskHandle_t  s_poll_task  = NULL;
static TaskHandle_t  s_pair_start_task = NULL;
static bool          s_pair_sensor_claimed = false;

static void start_hub_pairing(void)
{
    ESP_LOGI(TAG, "Starting hub BLE provisioning from mode %d", g_mode);
    g_mode = MODE_HUB_PAIRING;
    ESP_LOGI(TAG, "Mode transition: HUB_PAIRING");
    display_show("HUB PAIRING", "Connect via BLE");
    ble_start();
}

static void pairing_timeout_cb(TimerHandle_t xTimer)
{
    // NOTE: do not call display_show or ESP_LOGI here — display_show calls ESP_LOGI
    // internally, and ESP_LOGI's printf chain overflows the Tmr Svc task stack (~2KB).
    // Poll task detects g_mode change and exits on its own.
    if (g_mode == MODE_SENSOR_PAIRING) {
        g_mode = MODE_OPERATIONAL;
    }
}

// Polls GET /api/device/hubs/pending-sensor every 3 s until one sensor is
// claimed. The remaining pairing window is reserved for the ESP-NOW handshake.
static void sensor_poll_task(void *arg)
{
    (void)arg;
    char sensor_mac[18]    = {0};
    char provision_key[33] = {0};
    char sensor_name[32]   = {0};
    char sensor_zone[32]   = {0};
    ESP_LOGI(TAG, "Sensor pairing poll task started: interval=%d ms timeout=%d ms",
             SENSOR_POLL_INTERVAL_MS, SENSOR_PAIR_TIMEOUT_MS);

    while (g_mode == MODE_SENSOR_PAIRING && !s_pair_sensor_claimed) {
        if (api_fetch_sensor_pairing(sensor_mac, provision_key,
                                     sensor_name, sizeof(sensor_name),
                                     sensor_zone, sizeof(sensor_zone))) {
            ESP_LOGI(TAG, "Pending sensor received: %s. Saving provisional NVS and starting ESP-NOW", sensor_mac);
            s_pair_sensor_claimed = true;
            nvs_prov_save_sensor(sensor_mac, provision_key, sensor_name, sensor_zone);
            espnow_pair_sensor(sensor_mac, provision_key, sensor_name, sensor_zone);
            memset(provision_key, 0, sizeof(provision_key));
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Sensor pairing poll task exiting, current mode=%d", g_mode);
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

static void sensor_pairing_start_task(void *arg)
{
    (void)arg;

    if (!api_enable_sensor_pairing()) {
        ESP_LOGW(TAG, "Sensor pairing server window failed; returning to operational mode");
        g_mode = MODE_OPERATIONAL;
        s_pair_sensor_claimed = false;
        display_show_dashboard(true);
        s_pair_start_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (s_pair_timer) {
        xTimerReset(s_pair_timer, pdMS_TO_TICKS(100));
    }

    if (s_poll_task == NULL) {
        if (xTaskCreate(sensor_poll_task, "sensor_poll", SENSOR_POLL_STACK, NULL, 5, &s_poll_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create sensor pairing poll task");
            s_poll_task = NULL;
            if (s_pair_timer) {
                xTimerStop(s_pair_timer, pdMS_TO_TICKS(100));
            }
            g_mode = MODE_OPERATIONAL;
            display_show_dashboard(true);
            s_pair_start_task = NULL;
            vTaskDelete(NULL);
            return;
        }
    } else {
        ESP_LOGI(TAG, "Sensor pairing poll task already running");
    }

    ESP_LOGI(TAG, "Sensor pairing window opened (2 min)");
    s_pair_start_task = NULL;
    vTaskDelete(NULL);
    return;
}

void sensor_pairing_open_window(void)
{
    if (g_mode != MODE_OPERATIONAL && g_mode != MODE_SENSOR_PAIRING) {
        ESP_LOGW(TAG, "Cannot open sensor pairing from mode %d", g_mode);
        return;
    }
    if (s_pair_start_task != NULL) {
        ESP_LOGI(TAG, "Sensor pairing start already in progress");
        return;
    }

    g_mode = MODE_SENSOR_PAIRING;
    s_pair_sensor_claimed = false;
    ESP_LOGI(TAG, "Mode transition: SENSOR_PAIRING");

    if (xTaskCreate(sensor_pairing_start_task, "sensor_pair_start",
                    SENSOR_PAIR_START_STACK, NULL, 5, &s_pair_start_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor pairing start task");
        s_pair_start_task = NULL;
        g_mode = MODE_OPERATIONAL;
        display_show_dashboard(true);
    }
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

                if (strlen(g_hub_secret) == 0 &&
                    (g_mode == MODE_IDLE || g_mode == MODE_OPERATIONAL || g_mode == MODE_OFFLINE)) {
                    start_hub_pairing();

                } else if (g_mode == MODE_OPERATIONAL && strlen(g_hub_secret) > 0) {
                    ESP_LOGI(TAG, "Registered hub button action: fingerprint gate before sensor pairing");
                    display_show_fingerprint_screen("Authentication", "Place your finger on the sensor");
                    g_mode = MODE_FINGERPRINT_VERIFY;
                    ESP_LOGI(TAG, "Mode transition: FINGERPRINT_VERIFY");
                    if (fp_verify() == ESP_OK) {
                        ESP_LOGI(TAG, "Fingerprint verified. Opening sensor pairing");
                        g_mode = MODE_OPERATIONAL;
                        sensor_pairing_open_window();
                    } else {
                        ESP_LOGW(TAG, "Fingerprint verification failed. Sensor pairing not opened");
                        g_mode = MODE_OPERATIONAL;
                        vTaskDelay(pdMS_TO_TICKS(1200));
                        display_show_dashboard(true);
                    }
                } else {
                    ESP_LOGW(TAG, "Button press ignored in mode %d", g_mode);
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
