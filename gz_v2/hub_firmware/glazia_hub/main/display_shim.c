#include "display.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "state.h"

#define UART_NUM UART_NUM_3
#define TX_PIN 22
#define RX_PIN 23
#define UART_BUF_SIZE 1024

static const char *TAG = "DISPLAY_SHIM";

extern void ble_start(void);
extern esp_err_t fp_verify(void);
extern esp_err_t fp_enroll(void);
extern esp_err_t fp_verify_admin(void);
extern bool wifi_resume_from_offline_mode(void);
extern void wifi_enter_offline_mode(void);

enum {
    AUTH_ACTION_HUB_TOGGLE = 1,
    AUTH_ACTION_ADD_SENSOR,
    AUTH_ACTION_ADD_FINGERPRINT,
};

typedef struct {
    int action;
    bool want_on;
} auth_arg_t;

static volatile bool s_auth_busy = false;

static void auth_task(void *arg)
{
    auth_arg_t a = *(auth_arg_t *)arg;
    free(arg);

    display_show_fingerprint_screen("Authentication", "Place your finger on the sensor");
    vTaskDelay(pdMS_TO_TICKS(350));

    esp_err_t result = (a.action == AUTH_ACTION_ADD_FINGERPRINT) ? fp_verify_admin() : fp_verify();

    bool is_online = (g_mode != MODE_OFFLINE);

    if (result == ESP_OK) {
        if (a.action == AUTH_ACTION_ADD_SENSOR) {
            g_mode = MODE_OPERATIONAL;
            display_clear_sensor_notifications();
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"cmd\":\"load_screen\",\"id\":8}");
            uart_write_bytes(UART_NUM, buf, strlen(buf));
            uart_write_bytes(UART_NUM, "\n", 1);
        } else if (a.action == AUTH_ACTION_ADD_FINGERPRINT) {
            display_show_fingerprint_screen("Registration", "Place your finger on the sensor");
            vTaskDelay(pdMS_TO_TICKS(350));
            g_mode = MODE_FINGERPRINT_ENROLL;
            display_fingerprint_status(fp_enroll() == ESP_OK ? "Registration completed" : "Denied");
            g_mode = is_online ? MODE_OPERATIONAL : MODE_OFFLINE;
            vTaskDelay(pdMS_TO_TICKS(1200));
            display_show_dashboard(is_online);
        } else if (a.want_on) {
            display_fingerprint_status("Turning hub on");
            bool ok = wifi_resume_from_offline_mode();
            display_show_dashboard(ok);
        } else {
            display_fingerprint_status("Turning hub off");
            wifi_enter_offline_mode();
            display_show_dashboard(false);
        }
    } else {
        display_fingerprint_status("Denied");
        vTaskDelay(pdMS_TO_TICKS(1200));
        display_show_dashboard(is_online);
    }

    s_auth_busy = false;
    vTaskDelete(NULL);
}

static void uart_rx_task(void *arg) {
    uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
    while (1) {
        int len = uart_read_bytes(UART_NUM, data, UART_BUF_SIZE - 1, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            cJSON *json = cJSON_Parse((char*)data);
            if (json) {
                cJSON *event = cJSON_GetObjectItem(json, "event");
                if (event && event->valuestring) {
                    if (strcmp(event->valuestring, "reg_welcome") == 0) {
                        g_mode = MODE_HUB_PAIRING;
                        // Call directly since it's non-blocking, or could be a wrapper.
                        // Actually, ble_start() doesn't block and is safe to call from here.
                        ble_start();
                    } else if (strcmp(event->valuestring, "critical_toggle") == 0) {
                        auth_arg_t *req = malloc(sizeof(auth_arg_t));
                        if (req) {
                            req->action = AUTH_ACTION_HUB_TOGGLE;
                            req->want_on = (g_mode == MODE_OFFLINE);
                            xTaskCreate(auth_task, "disp_auth", 6144, req, 5, NULL);
                        }
                    } else if (strcmp(event->valuestring, "add_fingerprint") == 0) {
                        auth_arg_t *req = malloc(sizeof(auth_arg_t));
                        if (req) {
                            req->action = AUTH_ACTION_ADD_FINGERPRINT;
                            req->want_on = true;
                            xTaskCreate(auth_task, "disp_auth", 6144, req, 5, NULL);
                        }
                    } else if (strcmp(event->valuestring, "add_sensor_auth") == 0) {
                        auth_arg_t *req = malloc(sizeof(auth_arg_t));
                        if (req) {
                            req->action = AUTH_ACTION_ADD_SENSOR;
                            req->want_on = true;
                            xTaskCreate(auth_task, "disp_auth", 6144, req, 5, NULL);
                        }
                    }
                }
                cJSON_Delete(json);
            }
        }
    }
}

void display_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_LOGI(TAG, "UART shim initialized");
    
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL);
}

static void send_json(const char *json_str) {
    uart_write_bytes(UART_NUM, json_str, strlen(json_str));
    uart_write_bytes(UART_NUM, "\n", 1);
}

bool display_wait_ready(uint32_t timeout_ms) {
    return true; 
}

void display_show(const char *line1, const char *line2) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"show\",\"l1\":\"%s\",\"l2\":\"%s\"}", line1 ? line1 : "", line2 ? line2 : "");
    send_json(buf);
}

void display_show_setup_prompt(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"show_setup\",\"mac\":\"%s\"}", g_hub_mac);
    send_json(buf);
}

void display_fingerprint_status(const char *message) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"fp_status\",\"msg\":\"%s\"}", message ? message : "");
    send_json(buf);
}

void display_hub_location(const char *home_name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"hub_loc\",\"val\":\"%s\"}", home_name ? home_name : "");
    send_json(buf);
}

void display_user_name(const char *user_name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"user_name\",\"val\":\"%s\"}", user_name ? user_name : "");
    send_json(buf);
}

void display_sensor_list(void) {
    send_json("{\"cmd\":\"sensor_list\"}");
}

void display_sensor_location(const char *mac_str) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"sensor_loc\",\"val\":\"%s\"}", mac_str ? mac_str : "");
    send_json(buf);
}

void display_show_dashboard(bool online) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"dashboard\",\"online\":%s}", online ? "true" : "false");
    send_json(buf);
}

void display_show_fingerprint_screen(const char *title, const char *prompt) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"fp_screen\",\"title\":\"%s\",\"prompt\":\"%s\"}", title ? title : "", prompt ? prompt : "");
    send_json(buf);
}

void display_fingerprint_phase(const char *phase, const char *message) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"fp_phase\",\"phase\":\"%s\",\"msg\":\"%s\"}", phase ? phase : "", message ? message : "");
    send_json(buf);
}

void display_fingerprint_progress(uint8_t percent) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"fp_prog\",\"pct\":%d}", percent);
    send_json(buf);
}

#include "metrics_history.h"

void display_update_temp_hum(float temp, float hum) {
    float t_mn = temp, t_av = temp, t_mx = temp;
    float h_mn = hum, h_av = hum, h_mx = hum;
    metrics_history_stats(METRIC_TEMP, &t_mn, &t_av, &t_mx);
    metrics_history_stats(METRIC_HUM, &h_mn, &h_av, &h_mx);

    char buf[256];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"temp_hum\",\"t\":%.2f,\"h\":%.2f,\"t_mn\":%.2f,\"t_av\":%.2f,\"t_mx\":%.2f,\"h_mn\":%.2f,\"h_av\":%.2f,\"h_mx\":%.2f}", 
             temp, hum, t_mn, t_av, t_mx, h_mn, h_av, h_mx);
    send_json(buf);
}

void display_update_aqi(float aqi, const char *state, uint16_t pm25) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"aqi\",\"a\":%.2f,\"s\":\"%s\",\"p\":%u}", aqi, state ? state : "", pm25);
    send_json(buf);
}

void display_refresh_sensor_nodes(void) {
    send_json("{\"cmd\":\"refresh_nodes\"}");
}

void display_set_thread_sensor_offline(const uint8_t eui64[8], bool offline) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"sensor_offline\",\"eui\":\"%02x%02x%02x%02x%02x%02x%02x%02x\",\"off\":%s}",
             eui64[0], eui64[1], eui64[2], eui64[3], eui64[4], eui64[5], eui64[6], eui64[7],
             offline ? "true" : "false");
    send_json(buf);
}

void display_update_sensor_count(void) {
    send_json("{\"cmd\":\"count\"}");
}

void display_sensor_added_notification(const char *name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"cmd\":\"added_notif\",\"name\":\"%s\"}", name ? name : "");
    send_json(buf);
}

void display_clear_sensor_notifications(void) {
    send_json("{\"cmd\":\"clear_notifs\"}");
}
