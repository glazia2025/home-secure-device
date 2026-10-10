#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "cJSON.h"
#include "display.h"
#include "ui/ui.h"

#define UART_NUM UART_NUM_3
#define TX_PIN 22
#define RX_PIN 23
#define UART_BUF_SIZE 2048

static const char *TAG = "DISPLAY_MAIN";

static void parse_json_command(cJSON *json) {
    cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    if (!cmd || !cmd->valuestring) return;

    if (strcmp(cmd->valuestring, "dashboard") == 0) {
        cJSON *online = cJSON_GetObjectItem(json, "online");
        if (online) display_show_dashboard(cJSON_IsTrue(online));
    }
    else if (strcmp(cmd->valuestring, "show_setup") == 0) {
        cJSON *mac = cJSON_GetObjectItem(json, "mac");
        display_show_setup_prompt(mac ? mac->valuestring : "");
    }
    else if (strcmp(cmd->valuestring, "show") == 0) {
        cJSON *l1 = cJSON_GetObjectItem(json, "l1");
        cJSON *l2 = cJSON_GetObjectItem(json, "l2");
        display_show(l1 ? l1->valuestring : "", l2 ? l2->valuestring : "");
    }
    else if (strcmp(cmd->valuestring, "fp_status") == 0) {
        cJSON *msg = cJSON_GetObjectItem(json, "msg");
        if (msg && msg->valuestring) display_fingerprint_status(msg->valuestring);
    }
    else if (strcmp(cmd->valuestring, "hub_loc") == 0) {
        cJSON *val = cJSON_GetObjectItem(json, "val");
        if (val && val->valuestring) display_hub_location(val->valuestring);
    }
    else if (strcmp(cmd->valuestring, "user_name") == 0) {
        cJSON *val = cJSON_GetObjectItem(json, "val");
        if (val && val->valuestring) display_user_name(val->valuestring);
    }
    else if (strcmp(cmd->valuestring, "sensor_loc") == 0) {
        cJSON *val = cJSON_GetObjectItem(json, "val");
        if (val && val->valuestring) display_sensor_location(val->valuestring);
    }
    else if (strcmp(cmd->valuestring, "fp_screen") == 0) {
        cJSON *title = cJSON_GetObjectItem(json, "title");
        cJSON *prompt = cJSON_GetObjectItem(json, "prompt");
        display_show_fingerprint_screen(title ? title->valuestring : "", prompt ? prompt->valuestring : "");
    }
    else if (strcmp(cmd->valuestring, "fp_phase") == 0) {
        cJSON *phase = cJSON_GetObjectItem(json, "phase");
        cJSON *msg = cJSON_GetObjectItem(json, "msg");
        display_fingerprint_phase(phase ? phase->valuestring : "", msg ? msg->valuestring : "");
    }
    else if (strcmp(cmd->valuestring, "fp_prog") == 0) {
        cJSON *pct = cJSON_GetObjectItem(json, "pct");
        if (pct) display_fingerprint_progress(pct->valueint);
    }
    else if (strcmp(cmd->valuestring, "temp_hum") == 0) {
        cJSON *t = cJSON_GetObjectItem(json, "t");
        cJSON *h = cJSON_GetObjectItem(json, "h");
        cJSON *t_mn = cJSON_GetObjectItem(json, "t_mn");
        cJSON *t_av = cJSON_GetObjectItem(json, "t_av");
        cJSON *t_mx = cJSON_GetObjectItem(json, "t_mx");
        cJSON *h_mn = cJSON_GetObjectItem(json, "h_mn");
        cJSON *h_av = cJSON_GetObjectItem(json, "h_av");
        cJSON *h_mx = cJSON_GetObjectItem(json, "h_mx");
        
        float v_t = t ? t->valuedouble : 0;
        float v_h = h ? h->valuedouble : 0;
        float v_t_mn = t_mn ? t_mn->valuedouble : v_t;
        float v_t_av = t_av ? t_av->valuedouble : v_t;
        float v_t_mx = t_mx ? t_mx->valuedouble : v_t;
        float v_h_mn = h_mn ? h_mn->valuedouble : v_h;
        float v_h_av = h_av ? h_av->valuedouble : v_h;
        float v_h_mx = h_mx ? h_mx->valuedouble : v_h;
        
        display_update_temp_hum(v_t, v_h, v_t_mn, v_t_av, v_t_mx, v_h_mn, v_h_av, v_h_mx);
    }
    else if (strcmp(cmd->valuestring, "aqi") == 0) {
        cJSON *a = cJSON_GetObjectItem(json, "a");
        cJSON *s = cJSON_GetObjectItem(json, "s");
        cJSON *p = cJSON_GetObjectItem(json, "p");
        if (a && s && p) display_update_aqi(a->valuedouble, s->valuestring, p->valueint);
    }
    else if (strcmp(cmd->valuestring, "sensor_list") == 0) {
        display_sensor_list();
    }
    else if (strcmp(cmd->valuestring, "ref_nodes") == 0) {
        display_refresh_sensor_nodes();
    }
    else if (strcmp(cmd->valuestring, "upd_scount") == 0) {
        display_update_sensor_count();
    }
    else if (strcmp(cmd->valuestring, "added_notif") == 0) {
        cJSON *name = cJSON_GetObjectItem(json, "name");
        if (name && name->valuestring) display_sensor_added_notification(name->valuestring);
    }
    else if (strcmp(cmd->valuestring, "clr_noti") == 0) {
        display_clear_sensor_notifications();
    }
    else if (strcmp(cmd->valuestring, "sensor_offline") == 0) {
        cJSON *eui = cJSON_GetObjectItem(json, "eui");
        cJSON *off = cJSON_GetObjectItem(json, "off");
        if (eui && eui->valuestring && off) {
            uint8_t mac[8] = {0};
            // Simplistic parse for eui if needed; but in display.c `display_set_thread_sensor_offline`
            // signature takes uint8_t eui64[8].
            // It's mostly visual, and we don't have sscanf easily for all 8 bytes.
            unsigned int m[8];
            if (sscanf(eui->valuestring, "%02x%02x%02x%02x%02x%02x%02x%02x", 
                &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7]) == 8) {
                for (int i=0; i<8; i++) mac[i] = (uint8_t)m[i];
                display_set_thread_sensor_offline(mac, cJSON_IsTrue(off));
            }
        }
    }
}

static void uart_listener_task(void *arg) {
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

    uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
    int pos = 0;
    
    while (1) {
        int len = uart_read_bytes(UART_NUM, data + pos, (UART_BUF_SIZE - 1) - pos, 50 / portTICK_PERIOD_MS);
        if (len > 0) {
            pos += len;
            data[pos] = '\0';
            
            // Look for newline character to process complete messages
            char *newline;
            char *current = (char*)data;
            while ((newline = strchr(current, '\n')) != NULL) {
                *newline = '\0';
                cJSON *json = cJSON_Parse(current);
                if (json) {
                    parse_json_command(json);
                    cJSON_Delete(json);
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON: %s", current);
                }
                current = newline + 1;
            }
            
            // Move remaining data to the beginning
            int remaining = pos - (current - (char*)data);
            if (remaining > 0) {
                memmove(data, current, remaining);
                pos = remaining;
            } else {
                pos = 0;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting glazia_display project on ESP32-P4");
    display_init(); // Init power, MIPI, GT911, LVGL and start UI
    
    xTaskCreate(uart_listener_task, "uart_rx", 8192, NULL, 5, NULL);
}
