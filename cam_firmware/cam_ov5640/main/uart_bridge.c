#include "uart_bridge.h"
#include "webrtc_cam.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UART_BRIDGE";

/* ── Pins / port ──────────────────────────────────────────────────────────── */
#define CAM_UART_NUM    UART_NUM_1
#define CAM_UART_TX     2           /* cam TX → hub RX  (was MISO) */
#define CAM_UART_RX     1           /* cam RX ← hub TX  (was MOSI) */
#define CAM_UART_BAUD   115200
#define CAM_UART_RX_BUF 8192        /* ring buffer; comfortably holds >2 max frames */

/* ── Module state ─────────────────────────────────────────────────────────── */
static bool s_started = false;

/* ── Parse inbound bytes from hub and dispatch to webrtc_cam ─────────────── */
static void dispatch_mosi(const uint8_t *buf, uint8_t type, uint16_t len)
{
    const char *payload = (const char *)buf;

    switch (type) {
    case CAM_MSG_IDLE:
        break;
    case CAM_MSG_WEBRTC_START: {
        char *tmp = malloc(len + 1);
        if (!tmp) { ESP_LOGE(TAG, "OOM parsing WEBRTC_START"); break; }
        memcpy(tmp, payload, len);
        tmp[len] = '\0';
        ESP_LOGI(TAG, "RX: WEBRTC_START (%u bytes)", len);
        webrtc_cam_start_from_json(tmp);
        free(tmp);
        break;
    }
    case CAM_MSG_STOP:
        ESP_LOGI(TAG, "RX: STOP");
        webrtc_cam_stop();
        break;
    case CAM_MSG_ANSWER: {
        char *tmp = malloc(len + 1);
        if (!tmp) { ESP_LOGE(TAG, "OOM parsing ANSWER"); break; }
        memcpy(tmp, payload, len);
        tmp[len] = '\0';
        ESP_LOGI(TAG, "RX: ANSWER (%u bytes)", len);
        webrtc_cam_on_answer(tmp, (int)len);
        free(tmp);
        break;
    }
    case CAM_MSG_ICE_TO_CAM: {
        char *tmp = malloc(len + 1);
        if (!tmp) { ESP_LOGE(TAG, "OOM parsing ICE_TO_CAM"); break; }
        memcpy(tmp, payload, len);
        tmp[len] = '\0';
        ESP_LOGI(TAG, "RX: ICE_TO_CAM (%u bytes): %.80s", len, tmp);
        webrtc_cam_on_ice(tmp, (int)len);
        free(tmp);
        break;
    }
    default:
        ESP_LOGW(TAG, "RX: unknown type 0x%02X len=%u", type, len);
        break;
    }
}

/* ── RX task: reads framed bytes and dispatches to webrtc_cam ────────────── */
static void uart_rx_task(void *arg)
{
    uint8_t b;
    uint8_t hdr[3];

    while (1) {
        /* Seek magic byte — silently discards noise or partial frames */
        if (uart_read_bytes(CAM_UART_NUM, &b, 1, portMAX_DELAY) != 1) continue;
        if (b != 0xCA) continue;

        /* Read type (1 byte) + length (2 bytes big-endian), 100 ms timeout */
        if (uart_read_bytes(CAM_UART_NUM, hdr, 3, pdMS_TO_TICKS(100)) != 3) continue;

        uint8_t  type = hdr[0];
        uint16_t len  = ((uint16_t)hdr[1] << 8) | hdr[2];

        if (len > CAM_UART_MAX_PL) {
            ESP_LOGW(TAG, "RX: payload len %u exceeds max — discarding type 0x%02X", len, type);
            continue;
        }

        uint8_t *buf = malloc(len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "RX OOM for type 0x%02X len=%u", type, len);
            continue;
        }

        if (len > 0) {
            int got = uart_read_bytes(CAM_UART_NUM, buf, len, pdMS_TO_TICKS(200));
            if (got != (int)len) {
                ESP_LOGW(TAG, "RX: short read %d/%u for type 0x%02X — discarding", got, len, type);
                free(buf);
                continue;
            }
        }
        buf[len] = '\0';

        dispatch_mosi(buf, type, len);
        free(buf);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void uart_bridge_start(void)
{
    uart_config_t cfg = {
        .baud_rate  = CAM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CAM_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CAM_UART_NUM, CAM_UART_TX, CAM_UART_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CAM_UART_NUM, CAM_UART_RX_BUF, 0, 0, NULL, 0));
    s_started = true;

    xTaskCreate(uart_rx_task, "uart_bridge_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART bridge ready (UART%d TX=%d RX=%d baud=%d)",
             CAM_UART_NUM, CAM_UART_TX, CAM_UART_RX, CAM_UART_BAUD);
}

void uart_bridge_send_msg(uint8_t type, const char *payload, uint16_t len)
{
    if (!s_started) {
        ESP_LOGW(TAG, "uart_bridge_send_msg: bridge not started");
        return;
    }

    uint16_t capped = (len > CAM_UART_MAX_PL) ? CAM_UART_MAX_PL : len;

    /* Single contiguous buffer → single uart_write_bytes call (atomic on wire) */
    uint8_t *frame = malloc(4 + capped);
    if (!frame) {
        ESP_LOGE(TAG, "OOM sending type 0x%02X", type);
        return;
    }
    frame[0] = 0xCA;
    frame[1] = type;
    frame[2] = (uint8_t)(capped >> 8);
    frame[3] = (uint8_t)(capped & 0xFF);
    if (capped && payload) memcpy(frame + 4, payload, capped);

    int written = uart_write_bytes(CAM_UART_NUM, frame, 4 + capped);
    free(frame);

    if (written < 0) {
        ESP_LOGW(TAG, "uart_bridge_send_msg: write failed for type 0x%02X", type);
    } else {
        ESP_LOGI(TAG, "TX: type=0x%02X payload_len=%u bytes=%d", type, capped, written);
    }
}
