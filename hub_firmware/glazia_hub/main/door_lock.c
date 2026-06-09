#include "door_lock.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DOOR_LOCK_GPIO_NUM 14
#define DOOR_LOCK_GPIO GPIO_NUM_14
#define RELAY_ON       0
#define RELAY_OFF      1
#define UNLOCK_PULSE_MS 3000
#define MAX_TOGGLE_HOLD_MS 10000

#if DOOR_LOCK_GPIO_NUM == 37
#error "GPIO37 is unsafe for the lock relay on this ESP32-S3 PSRAM/camera hub build"
#endif

static const char *TAG = "DOOR_LOCK";

static QueueHandle_t s_command_queue;
static TaskHandle_t s_task;
static door_lock_ack_sender_t s_ack_sender;
static bool s_gpio_ready;
static bool s_started;

static esp_err_t ensure_gpio_ready(void)
{
    if (s_gpio_ready) return ESP_OK;

    ESP_LOGI(TAG, "Door lock GPIO%d config starting", DOOR_LOCK_GPIO);

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << DOOR_LOCK_GPIO,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio pre-config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_set_level(DOOR_LOCK_GPIO, RELAY_OFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_level inactive failed: %s", esp_err_to_name(err));
        return err;
    }

    config.mode = GPIO_MODE_OUTPUT;
    err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio output config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_gpio_ready = true;
    ESP_LOGI(TAG, "Door lock GPIO%d initialized inactive HIGH", DOOR_LOCK_GPIO);
    return ESP_OK;
}

static esp_err_t set_trigger(bool enabled)
{
    esp_err_t err = ensure_gpio_ready();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Door lock GPIO%d -> %s", DOOR_LOCK_GPIO, enabled ? "ACTIVE LOW" : "inactive HIGH");
    return gpio_set_level(DOOR_LOCK_GPIO, enabled ? RELAY_ON : RELAY_OFF);
}

static void send_ack(const door_lock_command_t *command,
                     const char *status,
                     const char *lock_state,
                     const char *error)
{
    if (s_ack_sender) {
        s_ack_sender(command->command_id, status, lock_state, error);
    }
}

static bool is_auto_lock_open_command(const door_lock_command_t *command)
{
    return strcmp(command->mode, "auto_lock") == 0 && strcmp(command->action, "open") == 0;
}

static bool is_toggle_on_command(const door_lock_command_t *command)
{
    return strcmp(command->mode, "toggle") == 0 && strcmp(command->action, "on") == 0;
}

static bool is_off_command(const door_lock_command_t *command)
{
    return strcmp(command->mode, "toggle") == 0 && strcmp(command->action, "off") == 0;
}

static bool execute_command(const door_lock_command_t *command, door_lock_command_t *next_command)
{
    ESP_LOGI(TAG, "Command id=%s mode=%s action=%s duration=%" PRIu32,
             command->command_id, command->mode, command->action, command->duration_ms);

    if (is_auto_lock_open_command(command)) {
        esp_err_t err = set_trigger(true);
        if (err != ESP_OK) {
            send_ack(command, "failed", "locked", esp_err_to_name(err));
            return false;
        }

        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(UNLOCK_PULSE_MS);
        door_lock_command_t interrupt_command;
        while (1) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) {
                break;
            }

            if (xQueueReceive(s_command_queue, &interrupt_command, deadline - now) != pdTRUE) {
                break;
            }

            if (is_off_command(&interrupt_command)) {
                set_trigger(false);
                send_ack(&interrupt_command, "executed", "locked", NULL);
                send_ack(command, "executed", "locked", NULL);
                return false;
            }

            set_trigger(false);
            send_ack(command, "executed", "locked", NULL);
            *next_command = interrupt_command;
            return true;
        }

        set_trigger(false);
        send_ack(command, "executed", "locked", NULL);
        return false;
    }

    if (is_toggle_on_command(command)) {
        esp_err_t err = set_trigger(true);
        if (err != ESP_OK) {
            send_ack(command, "failed", "locked", esp_err_to_name(err));
            return false;
        }

        uint32_t hold_ms = command->duration_ms;
        if (hold_ms == 0 || hold_ms > MAX_TOGGLE_HOLD_MS) {
            hold_ms = MAX_TOGGLE_HOLD_MS;
        }

        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(hold_ms);
        door_lock_command_t interrupt_command;
        while (1) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) {
                break;
            }

            if (xQueueReceive(s_command_queue, &interrupt_command, deadline - now) != pdTRUE) {
                break;
            }

            if (is_off_command(&interrupt_command)) {
                set_trigger(false);
                send_ack(&interrupt_command, "executed", "locked", NULL);
                send_ack(command, "executed", "locked", NULL);
                return false;
            }

            set_trigger(false);
            send_ack(command, "executed", "locked", NULL);
            *next_command = interrupt_command;
            return true;
        }

        set_trigger(false);
        send_ack(command, "executed", "locked", NULL);
        return false;
    }

    if (is_off_command(command)) {
        esp_err_t err = set_trigger(false);
        send_ack(command, err == ESP_OK ? "executed" : "failed", "locked",
                 err == ESP_OK ? NULL : esp_err_to_name(err));
        return false;
    }

    send_ack(command, "failed", "locked", "Unsupported door lock command");
    return false;
}

static void door_lock_task(void *arg)
{
    (void)arg;

    door_lock_command_t command;
    while (1) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            bool has_next;
            do {
                door_lock_command_t next_command = {0};
                has_next = execute_command(&command, &next_command);
                if (has_next) {
                    command = next_command;
                }
            } while (has_next);
        }
    }
}

esp_err_t door_lock_init(void)
{
    return ensure_gpio_ready();
}

esp_err_t door_lock_start(door_lock_ack_sender_t ack_sender)
{
    s_ack_sender = ack_sender;

    if (!s_command_queue) {
        s_command_queue = xQueueCreate(4, sizeof(door_lock_command_t));
        if (!s_command_queue) {
            ESP_LOGE(TAG, "Failed to create command queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_task) {
        if (xTaskCreate(door_lock_task, "door_lock", 4096, NULL, 6, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create door lock task");
            return ESP_ERR_NO_MEM;
        }
    }

    s_started = true;
    ESP_LOGI(TAG, "Door lock pulse handler started; GPIO%d only pulses on command", DOOR_LOCK_GPIO);
    return ESP_OK;
}

esp_err_t door_lock_enqueue(const door_lock_command_t *command)
{
    if (!command || !s_command_queue || !s_started) return ESP_ERR_INVALID_STATE;

    if (xQueueSend(s_command_queue, command, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
