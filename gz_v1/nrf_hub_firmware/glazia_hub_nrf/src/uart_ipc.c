#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include "uart_ipc.h"

LOG_MODULE_REGISTER(glazia_hub_nrf_ipc, LOG_LEVEL_INF);

static const struct device *s_uart;
static ipc_cmd_cb_t         s_cb;
static K_MUTEX_DEFINE(s_tx_mutex);

/* ── ISR→thread handoff: completed frames queued here, processed in rx_proc_thread ── */
struct rx_frame { uint8_t type; uint16_t len; uint8_t data[256]; };
K_MSGQ_DEFINE(s_rx_msgq, sizeof(struct rx_frame), 4, 4);
/* 8192 (not 2048): the command handlers dispatched from this thread run heavy OpenThread FTD
 * work — otDatasetCreateNewNetwork() generates the network key + PSKc via mbedTLS crypto, and
 * otCommissionerStart()/AddJoiner() are stack-hungry too. 2048 B overflowed and corrupted
 * adjacent memory (froze the LED task -> solid red). 8192 matches CONFIG_MAIN_STACK_SIZE. */
static K_THREAD_STACK_DEFINE(s_rx_stack, 12288);
static struct k_thread s_rx_thread;

/* ── Async DMA RX double-buffer ─────────────────────────────────────────────
 * nRF UARTE is EasyDMA-based; the async API is Nordic's recommended path.
 * Two ping-pong buffers keep RX continuous; the 10 ms idle timeout flushes
 * short frames (a PING is only 5 bytes and would otherwise never fill a buffer). */
#define RX_BUF_SZ     64
#define RX_TIMEOUT_US 10000
static uint8_t s_rx_buf[2][RX_BUF_SZ];
static uint8_t s_rx_next;

static void rx_proc_thread(void *a, void *b, void *c)
{
    struct rx_frame f;
    while (1) {
        if (k_msgq_get(&s_rx_msgq, &f, K_FOREVER) == 0) {
            LOG_INF("IPC rx: type=0x%02x len=%u", f.type, f.len);
            if (s_cb) s_cb(f.type, f.data, f.len);
        }
    }
}

/* ── CRC-8 (poly 0x07) ───────────────────────────────────────────────────── */
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
    case PS_SOF:  if (b == 0xA5) s_p.st = PS_TYPE; break;
    case PS_TYPE: s_p.type = b; s_p.crc_acc = crc8_byte(0, b); s_p.st = PS_LEN0; break;
    case PS_LEN0: s_p.len  = b; s_p.crc_acc = crc8_byte(s_p.crc_acc, b); s_p.st = PS_LEN1; break;
    case PS_LEN1:
        s_p.len |= ((uint16_t)b << 8);
        s_p.crc_acc = crc8_byte(s_p.crc_acc, b);
        s_p.idx = 0;
        s_p.st = (s_p.len == 0) ? PS_CRC : PS_DATA;
        break;
    case PS_DATA:
        if (s_p.idx < sizeof(s_p.buf)) s_p.buf[s_p.idx] = b;
        s_p.crc_acc = crc8_byte(s_p.crc_acc, b);
        if (++s_p.idx >= s_p.len) s_p.st = PS_CRC;
        break;
    case PS_CRC:
        if (b == s_p.crc_acc) {
            struct rx_frame f = { .type = s_p.type, .len = s_p.len };
            if (s_p.len <= sizeof(f.data)) memcpy(f.data, s_p.buf, s_p.len);
            k_msgq_put(&s_rx_msgq, &f, K_NO_WAIT);
        } else {
            LOG_WRN("CRC mismatch: got 0x%02x expected 0x%02x", b, s_p.crc_acc);
        }
        s_p.st = PS_SOF;
        break;
    }
}

/* ── UART async DMA RX callback (runs in UARTE ISR context) ─────────────────
 * Keep this short: feed received bytes into the parser (which only does an
 * ISR-safe k_msgq_put on a complete frame) and hand buffers back to the DMA. */
static void uart_async_cb(const struct device *dev, struct uart_event *evt, void *ud)
{
    switch (evt->type) {
    case UART_RX_RDY:
        for (size_t i = 0; i < evt->data.rx.len; i++)
            parser_feed(evt->data.rx.buf[evt->data.rx.offset + i]);
        break;
    case UART_RX_BUF_REQUEST:
        uart_rx_buf_rsp(dev, s_rx_buf[s_rx_next], RX_BUF_SZ);
        s_rx_next ^= 1;
        break;
    case UART_RX_STOPPED:
        /* Framing/overrun/parity error (e.g. a connect-time line glitch). The driver will
         * follow with UART_RX_DISABLED, where we re-arm — just note it, never leave RX dead. */
        LOG_WRN("RX stopped, reason=%d", evt->data.rx_stop.reason);
        break;
    case UART_RX_DISABLED: {        /* re-arm so RX can never stay dead after an error */
        int err;
        do {
            err = uart_rx_enable(dev, s_rx_buf[0], RX_BUF_SZ, RX_TIMEOUT_US);
        } while (err == -EBUSY);
        s_rx_next = 1;
        break;
    }
    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void uart_ipc_init(ipc_cmd_cb_t cb)
{
    s_cb   = cb;
    s_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
    if (!device_is_ready(s_uart)) {
        LOG_ERR("UART1 not ready");
        return;
    }

    int err = uart_callback_set(s_uart, uart_async_cb, NULL);
    if (err) { LOG_ERR("uart_callback_set failed: %d", err); return; }

    err = uart_rx_enable(s_uart, s_rx_buf[0], RX_BUF_SZ, RX_TIMEOUT_US);
    if (err) { LOG_ERR("uart_rx_enable failed: %d", err); return; }
    s_rx_next = 1;

    k_thread_create(&s_rx_thread, s_rx_stack, K_THREAD_STACK_SIZEOF(s_rx_stack),
                    rx_proc_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&s_rx_thread, "ipc_rx");
    LOG_INF("IPC UART1 ready (async DMA, TX=D0/P0.02 RX=D1/P0.03)");
}

void uart_ipc_send(uint8_t type, const uint8_t *payload, uint16_t plen)
{
    uint8_t frame[5 + 256];
    size_t flen = build_frame(type, payload, plen > 256 ? 256 : plen, frame);
    k_mutex_lock(&s_tx_mutex, K_FOREVER);
    for (size_t i = 0; i < flen; i++) uart_poll_out(s_uart, frame[i]);
    k_mutex_unlock(&s_tx_mutex);
}

void ipc_send_net_up(uint8_t channel, uint16_t panid)
{
    uint8_t p[3] = { channel, panid & 0xFF, (panid >> 8) & 0xFF };
    uart_ipc_send(IPC_EVT_NET_UP, p, sizeof(p));
}

void ipc_send_net_down(void) { uart_ipc_send(IPC_EVT_NET_DOWN, NULL, 0); }

void ipc_send_sensor_joined(const uint8_t eui64[8])
{
    uart_ipc_send(IPC_EVT_SENSOR_JOINED, eui64, 8);
}

void ipc_send_sensor_data(const uint8_t eui64[8], const char *json)
{
    uint8_t buf[8 + 128];
    memcpy(buf, eui64, 8);
    size_t jlen = strlen(json);
    if (jlen > 127) jlen = 127;
    memcpy(buf + 8, json, jlen);
    buf[8 + jlen] = '\0';
    uart_ipc_send(IPC_EVT_SENSOR_DATA, buf, 8 + jlen + 1);
}

void ipc_send_comm_failed(const uint8_t eui64[8])
{
    uart_ipc_send(IPC_EVT_COMM_FAILED, eui64, 8);
}

void ipc_send_sensor_lost(const uint8_t eui64[8])
{
    uart_ipc_send(IPC_EVT_SENSOR_LOST, eui64, 8);
}

void ipc_send_sensor_online(const uint8_t eui64[8])
{
    uart_ipc_send(IPC_EVT_SENSOR_ONLINE, eui64, 8);
}

void ipc_send_child_list(const uint8_t *extaddrs, int n_children)
{
    if (n_children < 0) n_children = 0;
    if (n_children > 32) n_children = 32;   /* 32*8 = 256 payload cap */
    uart_ipc_send(IPC_EVT_CHILD_LIST, extaddrs, (uint16_t)(n_children * 8));
}
