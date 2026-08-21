#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/udp.h>
#include <openthread/ip6.h>
#include <openthread/instance.h>
#include "event_tx.h"
#include "thread_join.h"
#include "led_indicator.h"

LOG_MODULE_REGISTER(glazia_sensor_nrf_tx, LOG_LEVEL_INF);

#define SENSOR_PORT    5683   /* sensor -> hub events            */
#define DOWNLINK_PORT  5684   /* hub -> sensor control commands  */
#define CACHE_MAX      10
#define PAYLOAD_MAX    32

static otUdpSocket s_sock;
static bool        s_initialized = false;

/* Offline cache — survives soft reset, flushed after Thread join */
static char s_cache[CACHE_MAX][PAYLOAD_MAX];
static int  s_cache_head  = 0;
static int  s_cache_tail  = 0;
static int  s_cache_count = 0;

static void cache_push(const char *evt)
{
    if (s_cache_count >= CACHE_MAX) {
        s_cache_head = (s_cache_head + 1) % CACHE_MAX;
        s_cache_count--;
    }
    strncpy(s_cache[s_cache_tail], evt, PAYLOAD_MAX - 1);
    s_cache[s_cache_tail][PAYLOAD_MAX - 1] = '\0';
    s_cache_tail = (s_cache_tail + 1) % CACHE_MAX;
    s_cache_count++;
}

/* Downlink from the hub. Currently one command: {"cmd":"leave","id":"<eui64>"} — if the id matches
 * our own EUI64, the hub deleted us, so factory-reset (erases Thread creds) and reboot. On reboot we
 * re-derive our PSKd and retry joining, but the commissioner allowlist no longer has us, so we stay
 * off until re-paired. */
static void udp_recv_cb(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    char buf[64];
    uint16_t len = otMessageGetLength(msg) - otMessageGetOffset(msg);
    if (len == 0 || len >= sizeof(buf)) return;
    otMessageRead(msg, otMessageGetOffset(msg), buf, len);
    buf[len] = '\0';

    if (!strstr(buf, "\"cmd\":\"leave\"")) return;

    const char *p = strstr(buf, "\"id\":\"");
    if (!p) return;
    p += 6;
    if (strncmp(p, g_sensor_eui64_hex, 16) != 0) return;   /* not addressed to us */

    LOG_WRN("leave command received — factory resetting");
    struct openthread_context *ctx2 = openthread_get_default_context();
    otInstanceFactoryReset(ctx2->instance);   /* erases persistent info + reboots */
}

static bool do_send(const char *event_str)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"e\":\"%s\"}", g_sensor_eui64_hex, event_str);

    struct openthread_context *ctx = openthread_get_default_context();
    openthread_api_mutex_lock(ctx);
    otInstance *ot = ctx->instance;

    otMessage *msg = otUdpNewMessage(ot, NULL);
    if (!msg) { openthread_api_mutex_unlock(ctx); return false; }
    otMessageAppend(msg, json, strlen(json));

    otMessageInfo info = {0};
    /* ff03::1 — mesh-local all-nodes multicast */
    info.mPeerAddr.mFields.m8[0]  = 0xff;
    info.mPeerAddr.mFields.m8[1]  = 0x03;
    info.mPeerAddr.mFields.m8[15] = 0x01;
    info.mPeerPort    = SENSOR_PORT;
    info.mMulticastLoop = true;

    otError err = otUdpSend(ot, &s_sock, msg, &info);
    openthread_api_mutex_unlock(ctx);
    return (err == OT_ERROR_NONE);
}

void event_tx_init(void)
{
    struct openthread_context *ctx = openthread_get_default_context();
    openthread_api_mutex_lock(ctx);
    otUdpOpen(ctx->instance, &s_sock, udp_recv_cb, NULL);
    /* Bind to the downlink port so we receive the hub's ff03::1 "leave" multicast. Sends to the
     * hub's event port still work from this same socket. */
    otSockAddr bind_addr = {0};
    bind_addr.mPort = DOWNLINK_PORT;
    otUdpBind(ctx->instance, &s_sock, &bind_addr, OT_NETIF_THREAD);
    openthread_api_mutex_unlock(ctx);
    s_initialized = true;
}

void event_tx_send(const char *event_str)
{
    if (!g_thread_ready || !s_initialized) {
        cache_push(event_str);
        LOG_INF("cached '%s' (%d in cache)", event_str, s_cache_count);
        led_sensor_flash_cached();
        return;
    }
    if (do_send(event_str)) {
        LOG_INF("TX %s", event_str);
        led_sensor_flash_sent();
    } else {
        LOG_WRN("TX fail — cached '%s'", event_str);
        cache_push(event_str);
        led_sensor_flash_cached();
    }
}

void event_tx_flush_cache(void)
{
    while (s_cache_count > 0) {
        const char *evt = s_cache[s_cache_head];
        if (!do_send(evt)) break;
        s_cache_head = (s_cache_head + 1) % CACHE_MAX;
        s_cache_count--;
        k_sleep(K_MSEC(50));
    }
}
