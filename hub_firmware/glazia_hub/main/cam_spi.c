#include "cam_spi.h"
#include "hub_control_ws.h"
#include "state.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CAM_SPI";

/* ── Pins ─────────────────────────────────────────────────────────────────── */
#define CAM_SPI_HOST    SPI3_HOST
#define CAM_SPI_SCLK    4
#define CAM_SPI_MOSI    12
#define CAM_SPI_MISO    11
#define CAM_SPI_CS      13
#define CAM_SPI_DRDY    10          /* input: cam_esp DRDY output */
#define CAM_SPI_CLK_HZ  (1 * 1000 * 1000)   /* 1 MHz — MISO signal integrity over dupont wires */

/* ── Outbound message (heap payload, freed by relay_task after copy) ─────── */
typedef struct {
    uint8_t   type;
    uint16_t  payload_len;
    char     *payload;   /* heap-allocated; relay_task frees */
} cam_msg_t;

/* ── Module state ─────────────────────────────────────────────────────────── */
static spi_device_handle_t  s_spi_dev    = NULL;
static QueueHandle_t        s_tx_queue   = NULL;
static uint8_t             *s_tx_buf     = NULL;
static uint8_t             *s_rx_buf     = NULL;
static volatile bool        s_streaming  = false;   /* true between webrtc_start and stop */
static char                *s_pending_offer = NULL; /* cached offer JSON for WS retry */
static StackType_t         *s_relay_stack = NULL;
static StaticTask_t        *s_relay_tcb = NULL;

/* ── Build MOSI from a queued message into s_tx_buf ──────────────────────── */
static void build_tx(const cam_msg_t *msg)
{
    memset(s_tx_buf, 0, CAM_SPI_MSG_SIZE);
    if (!msg || msg->type == CAM_MSG_IDLE) return;
    s_tx_buf[0] = 0xCA;
    s_tx_buf[1] = msg->type;
    s_tx_buf[2] = (uint8_t)(msg->payload_len >> 8);
    s_tx_buf[3] = (uint8_t)(msg->payload_len & 0xFF);
    if (msg->payload_len > 0 && msg->payload) {
        uint16_t copy = msg->payload_len;
        if (copy > CAM_SPI_MSG_SIZE - 4) {
            ESP_LOGW(TAG, "Payload truncated from %u to %u bytes", copy, CAM_SPI_MSG_SIZE - 4);
            copy = CAM_SPI_MSG_SIZE - 4;
        }
        memcpy(s_tx_buf + 4, msg->payload, copy);
    }
}

/* ── Execute one full-duplex 4096-byte SPI transaction ───────────────────── */
static void do_transaction(void)
{
    spi_transaction_t t = {
        .length    = CAM_SPI_MSG_SIZE * 8,
        .rxlength  = CAM_SPI_MSG_SIZE * 8,
        .tx_buffer = s_tx_buf,
        .rx_buffer = s_rx_buf,
    };
    esp_err_t err = spi_device_transmit(s_spi_dev, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI transaction error: %s", esp_err_to_name(err));
    }
}

/* ── Parse MISO and forward signaling to server ───────────────────────────── */
static void dispatch_rx(void)
{
    if (s_rx_buf[0] != 0xCA) {
        if (s_streaming) {
            bool all_zero = true;
            for (int i = 0; i < 8; i++) {
                if (s_rx_buf[i] != 0x00) all_zero = false;
            }
            if (all_zero) {
                ESP_LOGI(TAG, "dispatch_rx: MISO all-zero — cam idle or MISO wire open");
            } else {
                ESP_LOGW(TAG, "dispatch_rx: bad magic 0x%02X. First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                         s_rx_buf[0], s_rx_buf[0], s_rx_buf[1], s_rx_buf[2], s_rx_buf[3],
                         s_rx_buf[4], s_rx_buf[5], s_rx_buf[6], s_rx_buf[7]);
            }
        }
        return;
    }
    uint8_t  type = s_rx_buf[1];
    uint16_t len  = ((uint16_t)s_rx_buf[2] << 8) | s_rx_buf[3];

    if (type == CAM_MSG_IDLE) return;
    if (len > CAM_SPI_MSG_SIZE - 4) {
        ESP_LOGW(TAG, "RX payload len %u exceeds max — discarding type 0x%02X", len, type);
        return;
    }

    char *payload = (char *)&s_rx_buf[4];

    switch (type) {
    case CAM_MSG_OFFER:
        ESP_LOGI(TAG, "RX: offer from cam_esp (%u bytes) — forwarding to server", len);
        if (len > 0) {
            char saved = payload[len];
            payload[len] = '\0';
            free(s_pending_offer);
            s_pending_offer = strdup(payload);   /* cache for WS reconnect retry */
            hub_control_ws_send_json(payload);
            payload[len] = saved;
        }
        break;
    case CAM_MSG_ICE_FROM_CAM:
        ESP_LOGI(TAG, "RX: ICE candidate from cam_esp (%u bytes) — forwarding to server", len);
        if (len > 0) {
            char saved = payload[len];
            payload[len] = '\0';
            hub_control_ws_send_json(payload);
            payload[len] = saved;
        }
        break;
    default:
        ESP_LOGW(TAG, "RX: unknown type 0x%02X len=%u — ignoring", type, len);
        break;
    }
}

/* ── Relay task: polls DRDY, runs SPI, dispatches ────────────────────────── */
static void relay_task_fn(void *arg)
{
    TickType_t last_blind_poll = xTaskGetTickCount();
    uint32_t   s_poll_count    = 0;

    while (1) {
        bool drdy    = gpio_get_level(CAM_SPI_DRDY) == 1;
        bool has_tx  = uxQueueMessagesWaiting(s_tx_queue) > 0;

        /* Blind periodic poll while streaming: fire every 500 ms regardless of
         * DRDY so a broken or unconnected DRDY wire does not permanently block
         * offer/ICE delivery. DRDY is the fast path; this is the fallback. */
        TickType_t now = xTaskGetTickCount();
        bool blind = s_streaming &&
                     (now - last_blind_poll) >= pdMS_TO_TICKS(500);

        if (!drdy && !has_tx && !blind) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (blind) {
            ESP_LOGI(TAG, "SPI blind poll #%u (drdy=%d)", (unsigned)++s_poll_count, (int)drdy);
        }

        last_blind_poll = now;

        cam_msg_t out = {0};
        bool got = (xQueueReceive(s_tx_queue, &out, 0) == pdTRUE);
        build_tx(got ? &out : NULL);
        if (got && out.payload) {
            free(out.payload);
            out.payload = NULL;
        }

        memset(s_rx_buf, 0, CAM_SPI_MSG_SIZE);
        do_transaction();
        
        bool drdy_was_lying = (drdy && s_rx_buf[0] != 0xCA);
        
        dispatch_rx();

        /* Longer cooldown after sending a command so cam has time to process
         * before hub resumes DRDY polling. Prevents DMA-cache stale-data floods. */
        if (drdy_was_lying) {
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(got ? 500 : 5));
        }
    }
}

/* ── Queue a message for the relay task to send ──────────────────────────── */
static void enqueue(uint8_t type, const char *payload, uint16_t len)
{
    if (!s_tx_queue) {
        ESP_LOGW(TAG, "SPI not initialized — dropping msg type 0x%02X", type);
        return;
    }

    cam_msg_t msg = {.type = type, .payload_len = 0, .payload = NULL};
    if (len > 0 && payload) {
        uint16_t capped = (len > CAM_SPI_MSG_SIZE - 4) ? CAM_SPI_MSG_SIZE - 4 : len;
        msg.payload = malloc(capped + 1);
        if (!msg.payload) {
            ESP_LOGE(TAG, "OOM queueing cam SPI msg type 0x%02X", type);
            return;
        }
        memcpy(msg.payload, payload, capped);
        msg.payload[capped] = '\0';
        msg.payload_len = capped;
    }

    if (xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(msg.payload);
        ESP_LOGW(TAG, "SPI TX queue full — dropping msg type 0x%02X", type);
    }
}

/* ── Resend cached offer after a WS reconnect ────────────────────────────── */
void cam_spi_resend_pending_offer(void)
{
    if (s_pending_offer) {
        ESP_LOGI(TAG, "Resending cached offer (%u bytes) to server after WS reconnect",
                 (unsigned)strlen(s_pending_offer));
        hub_control_ws_send_json(s_pending_offer);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void cam_spi_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = CAM_SPI_MOSI,
        .miso_io_num     = CAM_SPI_MISO,
        .sclk_io_num     = CAM_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = CAM_SPI_MSG_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CAM_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .mode           = 0,
        .clock_speed_hz = CAM_SPI_CLK_HZ,
        .spics_io_num   = CAM_SPI_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CAM_SPI_HOST, &dev, &s_spi_dev));

    /* DRDY: level-polled input, no ISR */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CAM_SPI_DRDY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* DMA buffers: must be in internal SRAM */
    s_tx_buf = heap_caps_malloc(CAM_SPI_MSG_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_rx_buf = heap_caps_malloc(CAM_SPI_MSG_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_tx_buf || !s_rx_buf) {
        ESP_LOGE(TAG, "DMA buffer alloc failed (need %d B internal ×2)", CAM_SPI_MSG_SIZE);
        return;
    }
    memset(s_tx_buf, 0, CAM_SPI_MSG_SIZE);
    memset(s_rx_buf, 0, CAM_SPI_MSG_SIZE);

    /* Queue holds pointers; each slot is one cam_msg_t (~12 bytes) */
    s_tx_queue = xQueueCreate(4, sizeof(cam_msg_t));
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "TX queue create failed");
        return;
    }

    /* Keep this long-lived task's stack out of scarce internal SRAM. */
    s_relay_stack = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    s_relay_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!s_relay_stack || !s_relay_tcb) {
        ESP_LOGE(TAG, "Failed to allocate cam_relay task in PSRAM");
        return;
    }
    TaskHandle_t relay = xTaskCreateStaticPinnedToCore(
        relay_task_fn, "cam_relay", 4096, NULL, 5,
        s_relay_stack, s_relay_tcb, 0);
    if (!relay) {
        ESP_LOGE(TAG, "cam_relay task create failed");
        return;
    }

    ESP_LOGI(TAG, "SPI relay init (SCLK=%d MOSI=%d MISO=%d CS=%d DRDY=%d internal_free=%u largest=%u)",
             CAM_SPI_SCLK, CAM_SPI_MOSI, CAM_SPI_MISO, CAM_SPI_CS, CAM_SPI_DRDY,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void cam_spi_webrtc_start(const char *ssid, const char *pass,
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

    ESP_LOGI(TAG, "cam_spi_webrtc_start: ssid=%.24s turn_user=%.16s",
             ssid ? ssid : "", turn_user ? turn_user : "");
    free(s_pending_offer);
    s_pending_offer = NULL;
    s_streaming = true;
    enqueue(CAM_MSG_WEBRTC_START, json_str, (uint16_t)strlen(json_str));
    free(json_str);
}

void cam_spi_webrtc_stop(void)
{
    ESP_LOGI(TAG, "cam_spi_webrtc_stop");
    s_streaming = false;
    free(s_pending_offer);
    s_pending_offer = NULL;
    enqueue(CAM_MSG_STOP, NULL, 0);
}

void cam_spi_relay_answer(const char *sdp_str)
{
    if (!sdp_str) return;
    ESP_LOGI(TAG, "Relaying SDP answer to cam_esp (%u bytes)", (unsigned)strlen(sdp_str));
    enqueue(CAM_MSG_ANSWER, sdp_str, (uint16_t)strlen(sdp_str));
}

void cam_spi_relay_ice_to_cam(const char *cand_str)
{
    if (!cand_str) return;
    ESP_LOGI(TAG, "Relaying ICE to cam_esp: %.80s", cand_str);
    enqueue(CAM_MSG_ICE_TO_CAM, cand_str, (uint16_t)strlen(cand_str));
}
