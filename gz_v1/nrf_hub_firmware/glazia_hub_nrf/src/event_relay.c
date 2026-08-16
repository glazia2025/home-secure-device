#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/ip6.h>
#include "event_relay.h"
#include "uart_ipc.h"

LOG_MODULE_REGISTER(glazia_hub_nrf_relay, LOG_LEVEL_INF);

#define SENSOR_UDP_PORT 5683

static otUdpSocket s_sock;

/* Extract IEEE EUI64 from Thread mesh-local IPv6 IID (IID = EUI64 XOR 0x02) */
static void iid_to_eui64(const otIp6Address *addr, uint8_t eui64[8])
{
    memcpy(eui64, addr->mFields.m8 + 8, 8);
    eui64[0] ^= 0x02;
}

static void udp_recv_cb(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    uint8_t buf[128];
    uint16_t len = otMessageGetLength(msg) - otMessageGetOffset(msg);
    if (len == 0 || len >= sizeof(buf)) return;
    otMessageRead(msg, otMessageGetOffset(msg), buf, len);
    buf[len] = '\0';

    uint8_t eui64[8];
    iid_to_eui64(&info->mPeerAddr, eui64);

    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", eui64[i]);
    LOG_INF("UDP from %s: %s", hex, buf);

    ipc_send_sensor_data(eui64, (const char *)buf);
}

void event_relay_init(void)
{
    struct openthread_context *ctx = openthread_get_default_context();
    openthread_api_mutex_lock(ctx);
    otInstance *ot = ctx->instance;

    otSockAddr bind_addr = {0};
    bind_addr.mPort = SENSOR_UDP_PORT;

    otUdpOpen(ot, &s_sock, udp_recv_cb, NULL);
    otUdpBind(ot, &s_sock, &bind_addr, OT_NETIF_THREAD);

    openthread_api_mutex_unlock(ctx);
    LOG_INF("UDP event relay listening on port %d", SENSOR_UDP_PORT);
}
