#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include "uart_ipc.h"
#include "thread_mgr.h"
#include "event_relay.h"
#include "led_indicator.h"

LOG_MODULE_REGISTER(glazia_hub_nrf, LOG_LEVEL_INF);

/* Block up to 3 s for picocom to assert DTR; proceeds unconditionally so production
 * boards (no host connected) are never stuck. Required because CONFIG_UART_LINE_CTRL=y
 * holds log output until DTR is asserted — without this, all boot logs are dropped. */
static void wait_for_console(void)
{
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;
    for (int i = 0; i < 30 && !dtr; i++) {
        uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
        k_msleep(100);
    }
}

static void on_ipc_cmd(uint8_t type, const uint8_t *payload, uint16_t len)
{
    switch (type) {
    case IPC_CMD_PING:
        LOG_INF("CMD_PING — sending PONG");
        uart_ipc_send(IPC_EVT_PONG, NULL, 0);
        led_hub_flash_ack_rdy();   /* blue: packet received → green: ACK sent */
        break;

    case IPC_CMD_NET_FORM:
        led_hub_flash_rx();        /* blue: packet received */
        LOG_INF("CMD_NET_FORM");
        thread_mgr_form_network();
        break;

    case IPC_CMD_NET_STATUS:
        led_hub_flash_rx();
        LOG_INF("CMD_NET_STATUS");
        thread_mgr_report_status();   /* reply with current NET_UP/NET_DOWN */
        break;

    case IPC_CMD_COMMISSION: {
        led_hub_flash_rx();
        /* payload: eui64[8] + pskd[9 null-term] + timeout_s[u16-LE] = 19 bytes */
        if (len < 19) break;
        const uint8_t *eui64   = payload;
        const char    *pskd    = (const char *)(payload + 8);
        uint16_t       timeout = (uint16_t)payload[17] | ((uint16_t)payload[18] << 8);
        thread_mgr_commission(eui64, pskd, timeout);
        break;
    }

    case IPC_CMD_SENSOR_DEL:
        led_hub_flash_rx();
        if (len >= 8) thread_mgr_remove_joiner(payload);
        break;

    default:
        LOG_WRN("unknown IPC cmd 0x%02x", type);
    }
}

int main(void)
{
    wait_for_console();

    /* Physical-layer wire test: 3 raw 0xFF bytes via uart_poll_out, independent of
     * any async/DMA setup. ESP32 rx_task logs "first byte from nRF on GPIO16: 0xff"
     * within ~1s if the D0(P0.02)→GPIO16 wire and GND are good. */
    {
        const struct device *raw = DEVICE_DT_GET(DT_NODELABEL(uart1));
        if (device_is_ready(raw)) {
            uart_poll_out(raw, 0xFF);
            uart_poll_out(raw, 0xFF);
            uart_poll_out(raw, 0xFF);
        }
    }

    led_hub_init();   /* 3× red blink at boot, starts LED task at DETACHED */
    LOG_INF("glazia_hub_nrf v1.0.0 starting");

    uart_ipc_init(on_ipc_cmd);
    LOG_INF("UART IPC init done — entering thread_mgr_init");
    thread_mgr_init();
    LOG_INF("thread_mgr_init done — entering event_relay_init");
    event_relay_init();

    /* Form the Thread network autonomously at boot. It's a self-contained 802.15.4 network —
     * independent of WiFi and the hub — so forming here (radio idle, uninterrupted) lets it
     * attach cleanly and fast. OpenThread then keeps it up on its own; the hub only observes
     * NET_UP. thread_mgr_form_network() is idempotent (guarded to run once). */
    LOG_INF("event_relay_init done — forming Thread network");
    thread_mgr_form_network();

    uint32_t tick = 0;
    while (1) {
        k_sleep(K_SECONDS(5));
        LOG_INF("nRF alive tick=%u", tick++);   /* nRF-side liveness only; no PONG */
    }
    return 0;
}
