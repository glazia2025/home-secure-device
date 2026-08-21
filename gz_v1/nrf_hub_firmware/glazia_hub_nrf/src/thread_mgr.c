#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/border_router.h>
#include <openthread/commissioner.h>
#include <openthread/ip6.h>
#include "thread_mgr.h"
#include "uart_ipc.h"
#include "led_indicator.h"
#include "event_relay.h"

LOG_MODULE_REGISTER(glazia_hub_nrf_thread, LOG_LEVEL_INF);

static struct openthread_context *s_ctx;

static const char *role_str(otDeviceRole r)
{
    switch (r) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "unknown";
    }
}

/* Pending joiner: an "Add Sensor" that arrived before the commissioner was ACTIVE.
 * Stashed here and flushed by commissioner_state_cb() once the petition completes. */
static uint8_t  s_pending_eui64[8];
static char     s_pending_pskd[9];
static uint16_t s_pending_timeout_s;
static bool     s_have_pending;

/* The commissioner join callback reports a *hashed* Joiner ID, not the raw factory EUI64, so it
 * cannot be sent to the server (which keys sensors by real EUI64). We keep the last EUI64 actually
 * passed to otCommissionerAddJoiner and report THAT on join/fail. Pairing is one-at-a-time on the
 * hub (server pending-sensor + s_pair_sensor_claimed gate), so a single slot is sufficient. */
static uint8_t  s_last_joiner_eui64[8];
static bool     s_have_last_joiner;

/* Add a joiner assuming the caller already holds (or must not take) the OT API mutex —
 * i.e. no locking here. Emits COMM_FAILED to the ESP on synchronous rejection. */
static void add_joiner_locked(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s)
{
    otExtAddress addr;
    memcpy(addr.m8, eui64, 8);
    otError err = otCommissionerAddJoiner(s_ctx->instance, &addr, pskd,
                                          (uint32_t)timeout_s * 1000);
    if (err == OT_ERROR_NONE) {
        memcpy(s_last_joiner_eui64, eui64, 8);
        s_have_last_joiner = true;
        LOG_INF("commission: eui64=%02x%02x%02x%02x%02x%02x%02x%02x PSKd=%s timeout=%ds",
                eui64[0], eui64[1], eui64[2], eui64[3],
                eui64[4], eui64[5], eui64[6], eui64[7], pskd, timeout_s);
    } else {
        LOG_ERR("AddJoiner failed: %d", err);
        ipc_send_comm_failed(eui64);
    }
}

/* Passive per-sensor liveness. Each SED child data-polls the hub every 1 s and registers a 120 s
 * child timeout (set on the sensor). When a child stops polling, OpenThread evicts it from the
 * child table after that timeout and fires CHILD_REMOVED here; a (re)attach fires CHILD_ADDED. We
 * relay both to the ESP by the child's factory EUI64 (its 802.15.4 extended address). Runs in OT
 * callback context — read the entry and UART-send only; no OT calls, no re-lock. */
static void neighbor_cb(otNeighborTableEvent event, const otNeighborTableEntryInfo *info)
{
    if (!info) return;
    /* mExtAddress is the child's Thread MLE ext address (= factory EUI64 with the U/L bit 0x02
     * inverted). Log the raw bytes so we can confirm on HW it differs from the sensor's factory
     * EUI64 by only that bit — the ESP resolves it back to the stored EUI64 (U/L bit masked). */
    const uint8_t *m = info->mInfo.mChild.mExtAddress.m8;
    if (event == OT_NEIGHBOR_TABLE_EVENT_CHILD_REMOVED) {
        LOG_WRN("child removed (sensor lost): %02x%02x%02x%02x%02x%02x%02x%02x",
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        ipc_send_sensor_lost(m);
    } else if (event == OT_NEIGHBOR_TABLE_EVENT_CHILD_ADDED) {
        LOG_INF("child added (sensor online): %02x%02x%02x%02x%02x%02x%02x%02x",
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        ipc_send_sensor_online(m);
    }
}

/* ── Active child-table poll ───────────────────────────────────────────────────
 * The passive neighbor_cb (CHILD_REMOVED) proved unreliable, so we ALSO read the child table
 * directly on a timer and ship the full snapshot to the ESP. Reading the table depends only on
 * OpenThread's child-timeout eviction (what actually removes a dead SED), not on any event
 * callback. Each SED data-polls every 1 s to stay attached, so a present child = a live sensor;
 * no extra radio/battery — this only reads existing state and sends it over the on-board UART. */
#define CHILD_POLL_MS 3000

static void child_monitor_thread(void *a, void *b, void *c)
{
    static uint8_t list[32 * 8];
    while (1) {
        k_sleep(K_MSEC(CHILD_POLL_MS));
        if (!s_ctx) continue;

        int count = 0;
        openthread_api_mutex_lock(s_ctx);
        uint16_t max = otThreadGetMaxAllowedChildren(s_ctx->instance);
        for (uint16_t i = 0; i < max && count < 32; i++) {
            otChildInfo info;
            if (otThreadGetChildInfoByIndex(s_ctx->instance, i, &info) != OT_ERROR_NONE) continue;
            memcpy(&list[count * 8], info.mExtAddress.m8, 8);
            count++;
        }
        openthread_api_mutex_unlock(s_ctx);

        LOG_INF("child poll: %d present", count);
        ipc_send_child_list(list, count);
    }
}

static K_THREAD_STACK_DEFINE(s_child_stack, 4096);
static struct k_thread s_child_thread;

static void state_cb(otChangedFlags flags, void *ctx)
{
    struct openthread_context *ot_ctx = (struct openthread_context *)ctx;
    openthread_api_mutex_lock(ot_ctx);
    otDeviceRole role    = otThreadGetDeviceRole(ot_ctx->instance);
    uint8_t      channel = otLinkGetChannel(ot_ctx->instance);
    uint16_t     panid   = otLinkGetPanId(ot_ctx->instance);
    openthread_api_mutex_unlock(ot_ctx);

    LOG_INF("Thread role: %s  ch=%d pan=0x%04x", role_str(role), channel, panid);

    if (role >= OT_DEVICE_ROLE_CHILD) {
        led_hub_set_state(LED_HUB_NETWORK_UP);
        ipc_send_net_up(channel, panid);
    } else {
        led_hub_set_state(LED_HUB_DETACHED);
        if (role == OT_DEVICE_ROLE_DETACHED)
            ipc_send_net_down();
    }
}

static void commissioner_state_cb(otCommissionerState state, void *ctx)
{
    static const char *const names[] = { "disabled", "petitioning", "active" };
    LOG_INF("Commissioner: %s", state < 3 ? names[state] : "unknown");
    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        led_hub_set_state(LED_HUB_COMMISSIONING);
        /* Session is up — flush any join that was deferred while petitioning.
         * This runs in OpenThread callback context, so the API mutex is already
         * effectively held: call AddJoiner directly, never re-lock (would deadlock). */
        if (s_have_pending) {
            s_have_pending = false;
            add_joiner_locked(s_pending_eui64, s_pending_pskd, s_pending_timeout_s);
        }
    } else if (state == OT_COMMISSIONER_STATE_DISABLED) {
        led_hub_set_state(LED_HUB_NETWORK_UP);
    }
}

static bool s_finalized;

static void joiner_cb(otCommissionerJoinerEvent event,
                      const otJoinerInfo *info,
                      const otExtAddress *eui64,
                      void *ctx)
{
    if (!eui64) return;

    /* Report the real factory EUI64 we added, not the callback's hashed Joiner ID (which the
     * server can't match). Fall back to the callback ID only if we somehow have no record. */
    const uint8_t *id = s_have_last_joiner ? s_last_joiner_eui64 : eui64->m8;

    if (event == OT_COMMISSIONER_JOINER_FINALIZE) {
        s_finalized = true;
        LOG_INF("sensor joined: %02x%02x%02x%02x%02x%02x%02x%02x",
                id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
        led_hub_flash_joined();
        ipc_send_sensor_joined(id);
    } else if (event == OT_COMMISSIONER_JOINER_REMOVED) {
        if (!s_finalized) {
            LOG_WRN("commission failed: %02x%02x%02x%02x%02x%02x%02x%02x",
                    id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
            led_hub_flash_comm_failed();
            ipc_send_comm_failed(id);
        }
        s_finalized = false;
    }
}

void thread_mgr_init(void)
{
    s_ctx = openthread_get_default_context();
    openthread_api_mutex_lock(s_ctx);
    otSetStateChangedCallback(s_ctx->instance, state_cb, s_ctx);
    otThreadRegisterNeighborTableCallback(s_ctx->instance, neighbor_cb);
    openthread_api_mutex_unlock(s_ctx);

    /* Active child-table poller: authoritative liveness source for the ESP watchdog. */
    k_thread_create(&s_child_thread, s_child_stack, K_THREAD_STACK_SIZEOF(s_child_stack),
                    child_monitor_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
    k_thread_name_set(&s_child_thread, "child_mon");

    LOG_INF("thread_mgr ready");
}

void thread_mgr_form_network(void)
{
    openthread_api_mutex_lock(s_ctx);
    otInstance *ot = s_ctx->instance;

    /* Form exactly once. Thread is only ever enabled here; once enabled the role leaves
     * DISABLED for good, so a second call (e.g. a stray/legacy CMD_NET_FORM) must be a no-op —
     * re-running the enable/attach sequence disturbs MLE leader-election and stalls attach. */
    if (otThreadGetDeviceRole(ot) != OT_DEVICE_ROLE_DISABLED) {
        LOG_INF("Thread already formed (role=%s) — ignoring", role_str(otThreadGetDeviceRole(ot)));
        openthread_api_mutex_unlock(s_ctx);
        return;
    }

    /* Only create a new dataset on first boot. If one already exists in
     * non-volatile storage, reuse it so previously joined sensors stay valid. */
    otOperationalDataset existing = {0};
    bool has_dataset = (otDatasetGetActive(ot, &existing) == OT_ERROR_NONE);

    if (!has_dataset) {
        otOperationalDataset ds = {0};
        if (otDatasetCreateNewNetwork(ot, &ds) != OT_ERROR_NONE) {
            LOG_ERR("dataset create failed");
            openthread_api_mutex_unlock(s_ctx);
            return;
        }
        otDatasetSetActive(ot, &ds);

        otBorderRouterConfig br = {0};
        br.mOnMesh     = true;
        br.mSlaac      = true;
        br.mPreference = OT_ROUTE_PREFERENCE_HIGH;
        memset(br.mPrefix.mPrefix.mFields.m8, 0xfd, 1);
        br.mPrefix.mLength = 64;
        otBorderRouterAddOnMeshPrefix(ot, &br);
        otBorderRouterRegister(ot);
        LOG_INF("new Thread network created");
    } else {
        LOG_INF("restoring Thread network from flash");
    }

    otThreadSetRouterEligible(ot, true);
    otIp6SetEnabled(ot, true);
    otThreadSetEnabled(ot, true);

    /* Become leader immediately instead of waiting out OpenThread's attach-then-promote search
     * (a fresh node otherwise spends ~30 s looking for a parent to join before self-promoting).
     * This hub is the sole founder / leader / border-router / commissioner of its own private
     * network, so forming the partition directly is correct and deterministic. */
    otError lerr = otThreadBecomeLeader(ot);
    if (lerr != OT_ERROR_NONE)
        LOG_WRN("BecomeLeader returned %d — will attach normally", lerr);

    /* Do NOT start the commissioner here — petitioning during the initial attach collides with
     * MLE leader-election and stalls the network. The commissioner is started on demand by
     * thread_mgr_commission() (Add Sensor), by which point the node is already leader and the
     * petition completes immediately. */

    openthread_api_mutex_unlock(s_ctx);
    LOG_INF("Thread network %s (leader + commissioner)", has_dataset ? "restored" : "forming");
}

void thread_mgr_report_status(void)
{
    openthread_api_mutex_lock(s_ctx);
    otDeviceRole role    = otThreadGetDeviceRole(s_ctx->instance);
    uint8_t      channel = otLinkGetChannel(s_ctx->instance);
    uint16_t     panid   = otLinkGetPanId(s_ctx->instance);
    openthread_api_mutex_unlock(s_ctx);

    /* Answer an on-demand CMD_NET_STATUS query. The async state_cb only fires on role *changes*,
     * so a hub that reboots while the nRF is already up would never otherwise learn the state. */
    if (role >= OT_DEVICE_ROLE_CHILD) {
        LOG_INF("status: up (role=%s ch=%d pan=0x%04x)", role_str(role), channel, panid);
        ipc_send_net_up(channel, panid);
    } else {
        LOG_INF("status: down (role=%s)", role_str(role));
        ipc_send_net_down();
    }
}

void thread_mgr_commission(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s)
{
    openthread_api_mutex_lock(s_ctx);
    otInstance *ot = s_ctx->instance;
    otCommissionerState cs = otCommissionerGetState(ot);

    /* AddJoiner is only valid once the commissioner is ACTIVE. Calling it before the petition
     * completes returns OT_ERROR_INVALID_STATE synchronously — the cause of the instant
     * "commissioning failed" on the first Add Sensor. Gate on state; defer + (re)start otherwise. */
    if (cs == OT_COMMISSIONER_STATE_ACTIVE) {
        add_joiner_locked(eui64, pskd, timeout_s);
        openthread_api_mutex_unlock(s_ctx);
        return;
    }

    /* Stash the joiner; commissioner_state_cb() flushes it when the session reaches ACTIVE. */
    memcpy(s_pending_eui64, eui64, 8);
    strncpy(s_pending_pskd, pskd, sizeof(s_pending_pskd) - 1);
    s_pending_pskd[sizeof(s_pending_pskd) - 1] = '\0';
    s_pending_timeout_s = timeout_s;
    s_have_pending = true;

    if (cs == OT_COMMISSIONER_STATE_DISABLED) {
        otError err = otCommissionerStart(ot, commissioner_state_cb, joiner_cb, NULL);
        if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
            LOG_ERR("CommissionerStart failed: %d", err);
            s_have_pending = false;
            openthread_api_mutex_unlock(s_ctx);
            ipc_send_comm_failed(eui64);
            return;
        }
        LOG_INF("commissioner starting — join deferred until active");
    } else {
        LOG_INF("commissioner petitioning — join deferred until active");
    }
    openthread_api_mutex_unlock(s_ctx);
}

void thread_mgr_remove_joiner(const uint8_t eui64[8])
{
    /* Remove from the commissioning allowlist so it can't silently re-join without re-pairing. */
    openthread_api_mutex_lock(s_ctx);
    otExtAddress addr;
    memcpy(addr.m8, eui64, 8);
    otCommissionerRemoveJoiner(s_ctx->instance, &addr);
    openthread_api_mutex_unlock(s_ctx);

    /* RemoveJoiner only edits the allowlist — it can't evict an already-joined node. Send a
     * "leave" downlink so the sensor factory-resets itself off the network. */
    event_relay_send_leave(eui64);
}
