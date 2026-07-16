#include "spi_bridge.h"
#include "webrtc_cam.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "SPI_BRIDGE";

/* ── Pins ─────────────────────────────────────────────────────────────────── */
#define CAM_SPI_HOST    SPI2_HOST
#define CAM_SPI_SCLK    14
#define CAM_SPI_MOSI    1       /* receives data from hub */
#define CAM_SPI_MISO    46      /* sends data to hub */
#define CAM_SPI_CS      21
#define CAM_SPI_DRDY    0       /* output: HIGH = cam has a message queued for hub */

/* ── Outbound message (cam→hub), queued by webrtc_cam ─────────────────────── */
typedef struct {
    uint8_t   type;
    uint16_t  payload_len;
    char     *payload;   /* heap-allocated; listener task frees after copying */
} cam_msg_t;

/* ── Module state ─────────────────────────────────────────────────────────── */
static QueueHandle_t s_tx_queue = NULL;   /* outbound signaling messages */

/* ── SPI slave bus/device config ──────────────────────────────────────────── */
static spi_bus_config_t s_bus = {
    .mosi_io_num     = CAM_SPI_MOSI,
    .miso_io_num     = CAM_SPI_MISO,
    .sclk_io_num     = CAM_SPI_SCLK,
    .quadwp_io_num   = -1,
    .quadhd_io_num   = -1,
    .max_transfer_sz = CAM_SPI_MSG_SIZE,
};
static spi_slave_interface_config_t s_slave = {
    .mode         = 0,
    .spics_io_num = CAM_SPI_CS,
    .queue_size   = 1,
    .flags        = 0,
};

static esp_err_t slave_init(void)
{
    return spi_slave_initialize(CAM_SPI_HOST, &s_bus, &s_slave, SPI_DMA_CH_AUTO);
}

static void slave_deinit(void)
{
    spi_slave_free(CAM_SPI_HOST);
}

/* ── Build MISO from queued outbound message ──────────────────────────────── */
static bool prepare_miso(uint8_t *tx_buf)
{
    memset(tx_buf, 0, CAM_SPI_MSG_SIZE);
    cam_msg_t out;
    if (xQueueReceive(s_tx_queue, &out, 0) != pdTRUE) {
        gpio_set_level(CAM_SPI_DRDY, 0);
        return false;
    }

    tx_buf[0] = 0xCA;
    tx_buf[1] = out.type;
    tx_buf[2] = (uint8_t)(out.payload_len >> 8);
    tx_buf[3] = (uint8_t)(out.payload_len & 0xFF);
    if (out.payload_len > 0 && out.payload) {
        uint16_t copy = out.payload_len;
        if (copy > CAM_SPI_MSG_SIZE - 4) copy = CAM_SPI_MSG_SIZE - 4;
        memcpy(tx_buf + 4, out.payload, copy);
    }
    free(out.payload);

    /* Assert DRDY to tell hub there is a message ready to clock out */
    gpio_set_level(CAM_SPI_DRDY, 1);
    
    return true;
}

/* ── Parse MOSI from hub and dispatch to webrtc_cam ──────────────────────── */
static void dispatch_mosi(const uint8_t *rx_buf)
{
    if (rx_buf[0] != 0xCA) return;
    uint8_t  type = rx_buf[1];
    uint16_t len  = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
    if (len > CAM_SPI_MSG_SIZE - 4) {
        ESP_LOGW(TAG, "MOSI payload len %u too large — discarding type 0x%02X", len, type);
        return;
    }

    const char *payload = (const char *)&rx_buf[4];

    switch (type) {
    case CAM_MSG_IDLE:
        break;
    case CAM_MSG_WEBRTC_START: {
        /* payload: JSON {"ssid":"...","pass":"...","turn_user":"...","turn_psw":"..."} */
        char *buf = malloc(len + 1);
        if (!buf) { ESP_LOGE(TAG, "OOM parsing WEBRTC_START"); break; }
        memcpy(buf, payload, len);
        buf[len] = '\0';
        ESP_LOGI(TAG, "RX: WEBRTC_START (%u bytes)", len);
        webrtc_cam_start_from_json(buf);
        free(buf);
        break;
    }
    case CAM_MSG_STOP:
        ESP_LOGI(TAG, "RX: STOP");
        webrtc_cam_stop();
        break;
    case CAM_MSG_ANSWER: {
        /* payload: raw SDP string */
        char *buf = malloc(len + 1);
        if (!buf) { ESP_LOGE(TAG, "OOM parsing ANSWER"); break; }
        memcpy(buf, payload, len);
        buf[len] = '\0';
        ESP_LOGI(TAG, "RX: ANSWER (%u bytes)", len);
        webrtc_cam_on_answer(buf, (int)len);
        free(buf);
        break;
    }
    case CAM_MSG_ICE_TO_CAM: {
        /* payload: raw ICE candidate string */
        char *buf = malloc(len + 1);
        if (!buf) { ESP_LOGE(TAG, "OOM parsing ICE_TO_CAM"); break; }
        memcpy(buf, payload, len);
        buf[len] = '\0';
        ESP_LOGI(TAG, "RX: ICE_TO_CAM (%u bytes): %.80s", len, buf);
        webrtc_cam_on_ice(buf, (int)len);
        free(buf);
        break;
    }
    default:
        ESP_LOGW(TAG, "RX: unknown type 0x%02X len=%u", type, len);
        break;
    }
}

/* ── Main SPI listener task ───────────────────────────────────────────────── */
static void spi_listener_task(void *arg)
{
    ESP_ERROR_CHECK(slave_init());

    gpio_config_t drdy_cfg = {
        .pin_bit_mask = (1ULL << CAM_SPI_DRDY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&drdy_cfg));
    gpio_set_level(CAM_SPI_DRDY, 0);

    uint8_t *tx_buf = heap_caps_malloc(CAM_SPI_MSG_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *rx_buf = heap_caps_malloc(CAM_SPI_MSG_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!tx_buf || !rx_buf) {
        ESP_LOGE(TAG, "DMA buffer alloc failed (need %d B internal ×2)", CAM_SPI_MSG_SIZE);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SPI slave ready (SPI2 SCLK=%d MOSI=%d MISO=%d CS=%d DRDY=%d)",
             CAM_SPI_SCLK, CAM_SPI_MOSI, CAM_SPI_MISO, CAM_SPI_CS, CAM_SPI_DRDY);

    /* pending_tx: true when tx_buf holds a message that hasn't been delivered yet.
     * Kept across iterations so a timeout doesn't lose the message. */
    bool pending_tx = false;

    while (1) {
        /* Only dequeue a new message if the previous one was delivered (or none pending) */
        if (!pending_tx) {
            pending_tx = prepare_miso(tx_buf);
            if (!pending_tx) memset(tx_buf, 0, CAM_SPI_MSG_SIZE);
        }

        if (pending_tx) {
            ESP_LOGI(TAG, "MISO loaded: magic=0x%02X type=0x%02X len=%u — awaiting hub poll",
                     tx_buf[0], tx_buf[1],
                     (unsigned)(((uint16_t)tx_buf[2] << 8) | tx_buf[3]));
        }

        /* Clear rx_buf before every transaction so aborted/partial transactions
         * (CS glitch, wire noise) don't leave stale MOSI bytes for dispatch_mosi. */
        memset(rx_buf, 0, CAM_SPI_MSG_SIZE);

        spi_slave_transaction_t t = {
            .length    = (size_t)CAM_SPI_MSG_SIZE * 8,
            .tx_buffer = tx_buf,
            .rx_buffer = rx_buf,
        };

        /* Block forever for hub to initiate a transaction. */
        esp_err_t err = spi_slave_transmit(CAM_SPI_HOST, &t, portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI slave transmit error: %s — reinitialising", esp_err_to_name(err));
            slave_deinit();
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_ERROR_CHECK(slave_init());
            pending_tx = false;
            continue;
        }

        /* If we delivered a real message, clear DRDY */
        if (pending_tx) {
            gpio_set_level(CAM_SPI_DRDY, 0);
        }

        if (rx_buf[0] != 0x00 || rx_buf[1] != CAM_MSG_IDLE) {
            ESP_LOGI(TAG, "Slave tx complete — MOSI magic=0x%02X type=0x%02X",
                     rx_buf[0], rx_buf[1]);
        }

        /* Transaction completed — message was delivered, ready for next one */
        pending_tx = false;

        /* Parse what hub sent in MOSI */
        dispatch_mosi(rx_buf);
    }

    heap_caps_free(tx_buf);
    heap_caps_free(rx_buf);
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void spi_bridge_start(void)
{
    s_tx_queue = xQueueCreate(4, sizeof(cam_msg_t));
    configASSERT(s_tx_queue);

    xTaskCreate(spi_listener_task, "spi_listener", 5120, NULL, 5, NULL);
}

void spi_bridge_queue_msg(uint8_t type, const char *payload, uint16_t len)
{
    if (!s_tx_queue) {
        ESP_LOGW(TAG, "spi_bridge_queue_msg: bridge not started");
        return;
    }

    cam_msg_t msg = {.type = type, .payload_len = 0, .payload = NULL};
    if (len > 0 && payload) {
        uint16_t capped = (len > CAM_SPI_MSG_SIZE - 4) ? CAM_SPI_MSG_SIZE - 4 : len;
        msg.payload = malloc(capped + 1);
        if (!msg.payload) {
            ESP_LOGE(TAG, "OOM queuing SPI msg type 0x%02X", type);
            return;
        }
        memcpy(msg.payload, payload, capped);
        msg.payload[capped] = '\0';
        msg.payload_len = capped;
    }

    if (xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(msg.payload);
        ESP_LOGW(TAG, "SPI TX queue full — dropping type 0x%02X", type);
        return;
    }

    /* Signal hub that we have data */
    gpio_set_level(CAM_SPI_DRDY, 1);
}
