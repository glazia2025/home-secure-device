/**
 * S3 Display Controller
 *
 * Sends display commands to the C6 over UART1.
 * Protocol: FILL:R,G,B | TEXT:message | CLEAR
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "s3_master";

#define UART_PORT   UART_NUM_1
#define UART_TX_PIN 17
#define UART_RX_PIN 18
#define UART_BAUD   115200

static void send_cmd(const char *cmd)
{
    uart_write_bytes(UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(UART_PORT, "\n", 1);
    ESP_LOGI(TAG, "→ %s", cmd);
}

void app_main(void)
{
    ESP_LOGI(TAG, "S3 display controller starting");
    ESP_LOGI(TAG, "TX=GPIO%d  RX=GPIO%d  baud=%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD);

    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 512, 512, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    vTaskDelay(pdMS_TO_TICKS(500)); /* let C6 finish booting */

    while (1) {
        send_cmd("FILL:255,0,0");       /* red   */
        vTaskDelay(pdMS_TO_TICKS(2000));

        send_cmd("FILL:0,255,0");       /* green */
        vTaskDelay(pdMS_TO_TICKS(2000));

        send_cmd("FILL:0,0,255");       /* blue  */
        vTaskDelay(pdMS_TO_TICKS(2000));

        send_cmd("TEXT:xio is great");  /* text  */
        vTaskDelay(pdMS_TO_TICKS(2000));

        send_cmd("CLEAR");              /* black */
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
