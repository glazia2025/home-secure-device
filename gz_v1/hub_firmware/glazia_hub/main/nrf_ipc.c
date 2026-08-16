#include "nrf_ipc.h"
#include "state.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "NRF_IPC";

static nrf_ipc_event_cb_t s_cb;
static SemaphoreHandle_t  s_tx_mutex;

/* Completed frames are handed from the tiny nrf_rx reader task to a large worker
 * task (nrf_evt) via this queue. on_ipc_event() does NVS + HTTPS/TLS, which must
 * never run on the UART reader's small stack (that corrupts adjacent heap). */
struct ipc_frame { uint8_t type; uint16_t len; uint8_t buf[256]; };
static QueueHandle_t s_evt_q;

/* ── CRC-8 (poly 0x07) — matches gz_v2 nrf firmware ─────────────────────── */
static uint8_t crc8_byte(uint8_t crc, uint8_t d)
{
    crc ^= d;
    for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    return crc;
}

static uint8_t crc8_buf(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    while (n--) c = crc8_byte(c, *d++);
    return c;
}

/* ── Frame builder: [0xA5][TYPE][LEN_LO][LEN_HI][PAYLOAD][CRC8] ─────────── */
static size_t build_frame(uint8_t type, const uint8_t *payload, uint16_t plen, uint8_t *out)
{
    out[0] = 0xA5;
    out[1] = type;
    out[2] = plen & 0xFF;
    out[3] = (plen >> 8) & 0xFF;
    if (plen && payload) memcpy(out + 4, payload, plen);
    out[4 + plen] = crc8_buf(out + 1, 3 + plen);
    return 5 + plen;
}

/* ── Parser state machine ────────────────────────────────────────────────── */
typedef enum { PS_SOF, PS_TYPE, PS_LEN0, PS_LEN1, PS_DATA, PS_CRC } pstate_t;

static struct {
    pstate_t st;
    uint8_t  type;
    uint16_t len, idx;
    uint8_t  buf[256];
    uint8_t  crc_acc;
} s_p;

static void parser_feed(uint8_t b)
{
    switch (s_p.st) {
    case PS_SOF:
        if (b == 0xA5) s_p.st = PS_TYPE;
        break;
    case PS_TYPE:
        s_p.type    = b;
        s_p.crc_acc = crc8_byte(0, b);
        s_p.st      = PS_LEN0;
        break;
    case PS_LEN0:
        s_p.len     = b;
        s_p.crc_acc = crc8_byte(s_p.crc_acc, b);
        s_p.st      = PS_LEN1;
        break;
    case PS_LEN1:
        s_p.len    |= ((uint16_t)b << 8);
        s_p.crc_acc = crc8_byte(s_p.crc_acc, b);
        s_p.idx     = 0;
        s_p.st      = (s_p.len == 0) ? PS_CRC : PS_DATA;
        break;
    case PS_DATA:
        if (s_p.idx < sizeof(s_p.buf)) s_p.buf[s_p.idx] = b;
        s_p.crc_acc = crc8_byte(s_p.crc_acc, b);
        if (++s_p.idx >= s_p.len) s_p.st = PS_CRC;
        break;
    case PS_CRC:
        if (b == s_p.crc_acc) {
            /* Hand off to the worker task — never run on_ipc_event on nrf_rx's stack */
            struct ipc_frame f = { .type = s_p.type, .len = s_p.len };
            if (s_p.len <= sizeof(f.buf)) memcpy(f.buf, s_p.buf, s_p.len);
            if (s_evt_q) xQueueSend(s_evt_q, &f, 0);
        } else {
            ESP_LOGW(TAG, "CRC mismatch: got 0x%02x expected 0x%02x", b, s_p.crc_acc);
        }
        s_p.st = PS_SOF;
        break;
    }
}

/* ── RX task: reads UART ring buffer, feeds parser (stays lightweight) ────── */
static void rx_task(void *arg)
{
    uint8_t byte;
    while (1) {
        int n = uart_read_bytes(NRF_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (n == 1) parser_feed(byte);
    }
}

/* ── Worker task: runs on_ipc_event (NVS + HTTPS/TLS) off the reader stack ── */
static void evt_task(void *arg)
{
    struct ipc_frame f;
    while (1) {
        if (xQueueReceive(s_evt_q, &f, portMAX_DELAY) == pdTRUE) {
            if (s_cb) s_cb(f.type, f.buf, f.len);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void nrf_ipc_init(nrf_ipc_event_cb_t cb)
{
    static bool s_initialized = false;
    if (s_initialized) {
        s_cb = cb;
        return;
    }
    s_initialized = true;

    s_cb       = cb;
    s_tx_mutex = xSemaphoreCreateMutex();
    s_evt_q    = xQueueCreate(4, sizeof(struct ipc_frame));

    uart_config_t cfg = {
        .baud_rate  = NRF_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Larger RX ring (2048) + high reader priority so PONGs/events are never dropped while
     * core 0 is busy with the WiFi/TLS/WSS burst. */
    uart_driver_install(NRF_UART_NUM, 2048, 512, 0, NULL, 0);
    uart_param_config(NRF_UART_NUM, &cfg);
    uart_set_pin(NRF_UART_NUM, NRF_UART_TX_GPIO, NRF_UART_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreatePinnedToCore(rx_task,  "nrf_rx",  4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(evt_task, "nrf_evt", 8192, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "IPC UART%d ready (TX=%d RX=%d %d baud)",
             NRF_UART_NUM, NRF_UART_TX_GPIO, NRF_UART_RX_GPIO, NRF_UART_BAUD);
}

void nrf_ipc_send(uint8_t type, const uint8_t *payload, uint16_t plen)
{
    if (!s_tx_mutex) return;              /* not initialised yet — drop */
    if (plen > 256) plen = 256;

    /* static (not on the caller's stack) — serialised by s_tx_mutex.
     * Keeps the 261 B buffer out of main_task's deep wifi-connect cascade. */
    static uint8_t frame[5 + 256];

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    size_t flen = build_frame(type, payload, plen, frame);
    uart_write_bytes(NRF_UART_NUM, frame, flen);
    xSemaphoreGive(s_tx_mutex);
}

void ipc_cmd_ping(void)
{
    nrf_ipc_send(IPC_CMD_PING, NULL, 0);
    ESP_LOGI(TAG, "CMD_PING sent — waiting for PONG from nRF");
}

void ipc_cmd_net_form(void)
{
    nrf_ipc_send(IPC_CMD_NET_FORM, NULL, 0);
    ESP_LOGI(TAG, "CMD_NET_FORM sent");
}

void ipc_cmd_commission(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s)
{
    uint8_t buf[8 + 9 + 2];
    memcpy(buf, eui64, 8);
    strncpy((char *)(buf + 8), pskd, 9);
    buf[8 + 8] = '\0';
    buf[17] = timeout_s & 0xFF;
    buf[18] = (timeout_s >> 8) & 0xFF;
    nrf_ipc_send(IPC_CMD_COMMISSION, buf, sizeof(buf));

    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", eui64[i]);
    ESP_LOGI(TAG, "CMD_COMMISSION eui64=%s pskd=%s timeout=%us", hex, pskd, timeout_s);
}

void ipc_cmd_sensor_del(const uint8_t eui64[8])
{
    nrf_ipc_send(IPC_CMD_SENSOR_DEL, eui64, 8);
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", eui64[i]);
    ESP_LOGI(TAG, "CMD_SENSOR_DEL eui64=%s", hex);
}
