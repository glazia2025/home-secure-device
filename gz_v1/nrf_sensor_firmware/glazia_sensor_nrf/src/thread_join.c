#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/joiner.h>
#include <openthread/link.h>
#include <openthread/ip6.h>
#include <openthread/dataset.h>
#include "thread_join.h"
#include "event_tx.h"
#include "led_indicator.h"

LOG_MODULE_REGISTER(glazia_sensor_nrf_join, LOG_LEVEL_INF);

#define RETRY_DELAY_S   30
/* Parent evicts us (fires CHILD_REMOVED at the hub) after this many s of missed 1 s data-polls —
 * i.e. the hub's dead-sensor detection latency. Test value 10 s (10 missed polls); widen later. */
#define CHILD_TIMEOUT_S 10

volatile bool g_thread_ready = false;
char g_sensor_eui64_hex[17] = {0};

static char  s_pskd[9];
static K_SEM_DEFINE(s_join_done, 0, 1);
static otError s_join_result;

static void derive_pskd(otInstance *ot)
{
    otExtAddress eui64;
    otLinkGetFactoryAssignedIeeeEui64(ot, &eui64);
    /* PSKd = last 4 bytes of EUI64 as uppercase hex — e.g. "AABBCCDD" */
    snprintf(s_pskd, sizeof(s_pskd), "%02X%02X%02X%02X",
             eui64.m8[4], eui64.m8[5], eui64.m8[6], eui64.m8[7]);
    /* Full EUI64 as lowercase hex — sent in every event so the hub identifies us by real EUI64 */
    for (int i = 0; i < 8; i++)
        snprintf(g_sensor_eui64_hex + i * 2, 3, "%02x", eui64.m8[i]);
    LOG_INF("EUI64 %02x%02x%02x%02x%02x%02x%02x%02x  PSKd=%s",
            eui64.m8[0], eui64.m8[1], eui64.m8[2], eui64.m8[3],
            eui64.m8[4], eui64.m8[5], eui64.m8[6], eui64.m8[7], s_pskd);
}

static void joiner_cb(otError result, void *ctx)
{
    s_join_result = result;
    k_sem_give(&s_join_done);
}

static K_THREAD_STACK_DEFINE(s_stack, 4096);
static struct k_thread s_thread;

/* True if a usable Thread dataset (network key present) is already stored in flash. The nRF
 * settings backend (CONFIG_SETTINGS_NVS) persists the dataset across reboots / battery swaps,
 * so a previously-commissioned sensor can re-attach without the hub reopening its commissioner. */
static bool has_stored_credentials(otInstance *ot)
{
    otOperationalDataset ds;
    if (otDatasetGetActive(ot, &ds) != OT_ERROR_NONE) return false;
    return ds.mComponents.mIsNetworkKeyPresent;
}

/* Re-attach path: no Joiner/commissioner needed. Enable Thread from the stored dataset and wait
 * for MLE to (re)attach us as a child. OpenThread retries attach on its own if the hub is not yet
 * in range, so a late-booting sensor simply attaches whenever the hub becomes reachable. */
static void attach_from_stored(struct openthread_context *ctx)
{
    openthread_api_mutex_lock(ctx);
    otInstance *ot = ctx->instance;
    otLinkSetPollPeriod(ot, 1000);          /* 1 s SED poll for responsive alerts + downlink */
    otThreadSetChildTimeout(ot, CHILD_TIMEOUT_S);
    otIp6SetEnabled(ot, true);
    otThreadSetEnabled(ot, true);
    openthread_api_mutex_unlock(ctx);

    while (1) {
        openthread_api_mutex_lock(ctx);
        otDeviceRole role = otThreadGetDeviceRole(ctx->instance);
        openthread_api_mutex_unlock(ctx);
        if (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
            role == OT_DEVICE_ROLE_LEADER) {
            break;
        }
        k_sleep(K_SECONDS(2));
    }
}

/* First-pairing path: run the Joiner until it succeeds. Requires the hub's commissioner window
 * to be open (Add Sensor). On success the dataset is persisted for subsequent boots. */
static void run_joiner(struct openthread_context *ctx)
{
    while (1) {
        openthread_api_mutex_lock(ctx);
        otInstance *ot = ctx->instance;
        otIp6SetEnabled(ot, true);
        otError start_err = otJoinerStart(ot, s_pskd, NULL,
                                          "glazia", NULL, "1.0.0", NULL,
                                          joiner_cb, NULL);
        openthread_api_mutex_unlock(ctx);

        if (start_err != OT_ERROR_NONE) {
            LOG_ERR("otJoinerStart error %d — retry in %ds", start_err, RETRY_DELAY_S);
            k_sleep(K_SECONDS(RETRY_DELAY_S));
            continue;
        }

        k_sem_take(&s_join_done, K_FOREVER);

        if (s_join_result == OT_ERROR_NONE) {
            LOG_INF("Thread join succeeded");
            openthread_api_mutex_lock(ctx);
            ot = ctx->instance;
            otLinkSetPollPeriod(ot, 1000);   /* 1 s SED poll for responsive alerts */
            otThreadSetChildTimeout(ot, CHILD_TIMEOUT_S);
            otThreadSetEnabled(ot, true);
            openthread_api_mutex_unlock(ctx);
            return;
        }

        LOG_WRN("Join failed (%d) — retry in %ds", s_join_result, RETRY_DELAY_S);
        k_sleep(K_SECONDS(RETRY_DELAY_S));
    }
}

static void joiner_task(void *a, void *b, void *c)
{
    struct openthread_context *ctx = openthread_get_default_context();

    /* Derive identity once: g_sensor_eui64_hex for event headers, s_pskd for the join path. */
    openthread_api_mutex_lock(ctx);
    derive_pskd(ctx->instance);
    bool has_creds = has_stored_credentials(ctx->instance);
    openthread_api_mutex_unlock(ctx);

    if (has_creds) {
        LOG_INF("re-attaching from stored credentials (no commissioner needed)");
        attach_from_stored(ctx);
    } else {
        LOG_INF("no stored credentials — joining via commissioner");
        run_joiner(ctx);
    }

    g_thread_ready = true;
    led_sensor_set_state(LED_SENSOR_READY);
    event_tx_flush_cache();
}

void thread_join_init(void)
{
    k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack),
                    joiner_task, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&s_thread, "joiner");
    LOG_INF("joiner task started");
}
