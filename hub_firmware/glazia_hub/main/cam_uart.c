#include "cam_uart.h"
#include "hub_control_ws.h"
#include "state.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CAM_UART";

/* ── Pins / port ──────────────────────────────────────────────────────────── */
#define CAM_UART_NUM    UART_NUM_1
#define CAM_UART_TX     12          /* hub TX → cam RX  (was MOSI) */
#define CAM_UART_RX     11          /* hub RX ← cam TX  (was MISO) */
#define CAM_UART_BAUD   115200
#define CAM_UART_RX_BUF 8192        /* ring buffer; comfortably holds >2 max frames */

/* ── Outbound message (heap payload, freed by TX task after write) ─────────── */
typedef struct {
    uint8_t   type;
    uint16_t  payload_len;
    char     *payload;   /* heap-allocated; TX task frees */
} cam_msg_t;

/* ── Module state ─────────────────────────────────────────────────────────── */
static QueueHandle_t  s_tx_queue     = NULL;
static volatile bool  s_streaming    = false;
static char          *s_pending_offer = NULL;   /* cached offer JSON for WS retry */

/* ── TX task: dequeues messages and writes framed bytes to UART ───────────── */
static void cam_uart_tx_task(void *arg)
{
    cam_msg_t msg;
    while (1) {
        xQueueReceive(s_tx_queue, &msg, portMAX_DELAY);

        /* Build one contiguous frame so uart_write_bytes is a single call
         * (ESP-IDF holds the tx mutex per call — one call = atomic on the wire). */
        uint8_t *frame = malloc(4 + msg.payload_len);
        if (!frame) {
            ESP_LOGE(TAG, "TX OOM for type 0x%02X payload_len=%u", msg.type, msg.payload_len);
            free(msg.payload);
            continue;
        }
        frame[0] = 0xCA;
        frame[1] = msg.type;
        frame[2] = (uint8_t)(msg.payload_len >> 8);
        frame[3] = (uint8_t)(msg.payload_len & 0xFF);
        if (msg.payload_len && msg.payload) {
            memcpy(frame + 4, msg.payload, msg.payload_len);
        }
        free(msg.payload);

        int written = uart_write_bytes(CAM_UART_NUM, frame, 4 + msg.payload_len);
        free(frame);

        if (written < 0) {
            ESP_LOGW(TAG, "uart_write_bytes failed for type 0x%02X", msg.type);
        } else {
            ESP_LOGI(TAG, "TX: type=0x%02X payload_len=%u bytes=%d",
                     msg.type, msg.payload_len, written);
        }
    }
}

/* ── RX task: reads framed bytes from UART and dispatches signaling ───────── */
static void cam_uart_rx_task(void *arg)
{
    uint8_t b;
    uint8_t hdr[3];

    while (1) {
        /* Seek magic byte — silently drops noise or partial frames */
        if (uart_read_bytes(CAM_UART_NUM, &b, 1, portMAX_DELAY) != 1) continue;
        if (b != 0xCA) continue;

        /* Read type (1 byte) + length (2 bytes big-endian), 100 ms timeout */
        if (uart_read_bytes(CAM_UART_NUM, hdr, 3, pdMS_TO_TICKS(100)) != 3) continue;

        uint8_t  type = hdr[0];
        uint16_t len  = ((uint16_t)hdr[1] << 8) | hdr[2];

        if (type == CAM_MSG_IDLE) continue;
        if (len > CAM_UART_MAX_PL) {
            ESP_LOGW(TAG, "RX: payload len %u exceeds max — discarding type 0x%02X", len, type);
            continue;
        }

        char *payload = malloc(len + 1);
        if (!payload) {
            ESP_LOGE(TAG, "RX OOM for type 0x%02X len=%u", type, len);
            continue;
        }

        if (len > 0) {
            int got = uart_read_bytes(CAM_UART_NUM, (uint8_t *)payload, len,
                                      pdMS_TO_TICKS(200));
            if (got != (int)len) {
                ESP_LOGW(TAG, "RX: short read %d/%u for type 0x%02X — discarding", got, len, type);
                free(payload);
                continue;
            }
        }
        payload[len] = '\0';

        switch (type) {
        case CAM_MSG_OFFER:
            ESP_LOGI(TAG, "RX: offer from cam_esp (%u bytes) — forwarding to server", len);
            free(s_pending_offer);
            s_pending_offer = strdup(payload);   /* cache for WS reconnect retry */
            hub_control_ws_send_json(payload);
            break;
        case CAM_MSG_ICE_FROM_CAM:
            ESP_LOGI(TAG, "RX: ICE candidate from cam_esp (%u bytes) — forwarding to server", len);
            hub_control_ws_send_json(payload);
            break;
        default:
            ESP_LOGW(TAG, "RX: unknown type 0x%02X len=%u — ignoring", type, len);
            break;
        }

        free(payload);
    }
}

/* ── Queue a message for the TX task ─────────────────────────────────────── */
static void enqueue(uint8_t type, const char *payload, uint16_t len)
{
    if (!s_tx_queue) {
        ESP_LOGW(TAG, "UART not initialized — dropping msg type 0x%02X", type);
        return;
    }

    cam_msg_t msg = {.type = type, .payload_len = 0, .payload = NULL};
    if (len > 0 && payload) {
        uint16_t capped = (len > CAM_UART_MAX_PL) ? CAM_UART_MAX_PL : len;
        msg.payload = malloc(capped + 1);
        if (!msg.payload) {
            ESP_LOGE(TAG, "OOM queueing cam UART msg type 0x%02X", type);
            return;
        }
        memcpy(msg.payload, payload, capped);
        msg.payload[capped] = '\0';
        msg.payload_len = capped;
    }

    if (xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(msg.payload);
        ESP_LOGW(TAG, "UART TX queue full — dropping msg type 0x%02X", type);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void cam_uart_resend_pending_offer(void)
{
    if (s_pending_offer) {
        ESP_LOGI(TAG, "Resending cached offer (%u bytes) to server after WS reconnect",
                 (unsigned)strlen(s_pending_offer));
        hub_control_ws_send_json(s_pending_offer);
    }
}

void cam_uart_init(void)
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

    s_tx_queue = xQueueCreate(4, sizeof(cam_msg_t));
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "TX queue create failed");
        return;
    }

    /* TX task: small stack — just queue receive + malloc + uart_write */
    xTaskCreatePinnedToCore(cam_uart_tx_task, "cam_uart_tx", 3072, NULL, 5, NULL, 0);
    /* RX task: larger stack — malloc for payloads up to CAM_UART_MAX_PL bytes */
    xTaskCreatePinnedToCore(cam_uart_rx_task, "cam_uart_rx", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "UART IPC ready (UART%d TX=%d RX=%d baud=%d)",
             CAM_UART_NUM, CAM_UART_TX, CAM_UART_RX, CAM_UART_BAUD);
}

void cam_uart_webrtc_start(const char *ssid, const char *pass,
                            const char *turn_user, const char *turn_psw)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid",      ssid      ? ssid      : "");
    cJSON_AddStringToObject(root, "pass",      pass      ? pass      : "");
    cJSON_AddStringToObject(root, "turn_user", turn_user ? turn_user : "");
    cJSON_AddStringToObject(root, "turn_psw",  turn_psw  ? turn_psw  : "");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to build WEBRTC_START payload");
        return;
    }

    ESP_LOGI(TAG, "cam_uart_webrtc_start: ssid=%.24s turn_user=%.16s",
             ssid ? ssid : "", turn_user ? turn_user : "");
    free(s_pending_offer);
    s_pending_offer = NULL;
    s_streaming = true;
    enqueue(CAM_MSG_WEBRTC_START, json_str, (uint16_t)strlen(json_str));
    free(json_str);
}

void cam_uart_webrtc_stop(void)
{
    ESP_LOGI(TAG, "cam_uart_webrtc_stop");
    s_streaming = false;
    free(s_pending_offer);
    s_pending_offer = NULL;
    enqueue(CAM_MSG_STOP, NULL, 0);
}

void cam_uart_relay_answer(const char *sdp_str)
{
    if (!sdp_str) return;
    ESP_LOGI(TAG, "Relaying SDP answer to cam_esp (%u bytes)", (unsigned)strlen(sdp_str));
    enqueue(CAM_MSG_ANSWER, sdp_str, (uint16_t)strlen(sdp_str));
}

void cam_uart_relay_ice_to_cam(const char *cand_str)
{
    if (!cand_str) return;
    ESP_LOGI(TAG, "Relaying ICE to cam_esp: %.80s", cand_str);
    enqueue(CAM_MSG_ICE_TO_CAM, cand_str, (uint16_t)strlen(cand_str));
}
