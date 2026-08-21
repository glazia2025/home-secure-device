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

#define SENSOR_UDP_PORT   5683   /* sensors -> hub events           */
#define DOWNLINK_UDP_PORT 5684   /* hub -> sensors control commands */

static otUdpSocket s_sock;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse the self-reported factory EUI64 from the JSON payload's "id" field into 8 bytes.
 * The sender's Thread mesh-local address uses a randomized ML-EID and can't be reversed to the
 * EUI64, so the sensor announces its real EUI64 in every event instead. Returns true on success. */
static bool parse_eui64_from_json(const char *json, uint8_t eui64[8])
{
    const char *p = strstr(json, "\"id\":\"");
    if (!p) return false;
    p += 6;
    for (int i = 0; i < 8; i++) {
        int hi = hex_nibble(p[i * 2]);
        int lo = hex_nibble(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        eui64[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void udp_recv_cb(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    uint8_t buf[128];
    uint16_t len = otMessageGetLength(msg) - otMessageGetOffset(msg);
    if (len == 0 || len >= sizeof(buf)) return;
    otMessageRead(msg, otMessageGetOffset(msg), buf, len);
    buf[len] = '\0';

    uint8_t eui64[8];
    if (!parse_eui64_from_json((const char *)buf, eui64)) {
        LOG_WRN("event missing/invalid \"id\" EUI64 — dropping: %s", buf);
        return;
    }

    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", eui64[i]);
    LOG_INF("UDP from %s: %s", hex, buf);

    ipc_send_sensor_data(eui64, (const char *)buf);
}

void event_relay_send_leave(const uint8_t eui64[8])
{
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", eui64[i]);

    char json[48];
    snprintf(json, sizeof(json), "{\"cmd\":\"leave\",\"id\":\"%s\"}", hex);

    struct openthread_context *ctx = openthread_get_default_context();
    openthread_api_mutex_lock(ctx);
    otInstance *ot = ctx->instance;

    otMessage *msg = otUdpNewMessage(ot, NULL);
    if (!msg) { openthread_api_mutex_unlock(ctx); LOG_WRN("leave: no msg buffer"); return; }
    otMessageAppend(msg, json, strlen(json));

    otMessageInfo info = {0};
    /* ff03::1 — mesh-local all-nodes multicast; only the addressed sensor acts on it. */
    info.mPeerAddr.mFields.m8[0]  = 0xff;
    info.mPeerAddr.mFields.m8[1]  = 0x03;
    info.mPeerAddr.mFields.m8[15] = 0x01;
    info.mPeerPort = DOWNLINK_UDP_PORT;

    otError err = otUdpSend(ot, &s_sock, msg, &info);
    openthread_api_mutex_unlock(ctx);
    if (err == OT_ERROR_NONE) LOG_INF("sent leave to %s", hex);
    else                      LOG_WRN("leave send failed: %d", err);
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
