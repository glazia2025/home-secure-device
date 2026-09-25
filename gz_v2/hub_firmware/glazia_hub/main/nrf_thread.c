#include "nrf_thread.h"
#include "nrf_ipc.h"
#include "nvs_storage.h"
#include "api_client.h"
#include "state.h"
#include "esp_log.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "NRF_THR";

volatile bool g_thread_net_ready = false;
static volatile bool s_ipc_alive  = false;

/* ── Cloud-event send lane ────────────────────────────────────────────────────
 * api_send_event() does a synchronous HTTPS POST that blocks ~2-4 s. It must NOT run on the IPC
 * worker (nrf_evt) — while blocked there, the child-table liveness snapshots pile up in the IPC
 * queue and are dropped, which is exactly what made healthy sensors look "not in mesh". So all
 * cloud events are handed to this dedicated sender task; the IPC worker returns immediately and
 * keeps draining membership snapshots. */
typedef struct { char sensor[17]; char type[24]; char severity[12]; } evt_send_t;
static QueueHandle_t s_event_q;

static void event_sender_task(void *arg)
{
    evt_send_t m;
    for (;;)
        if (xQueueReceive(s_event_q, &m, portMAX_DELAY) == pdTRUE)
            api_send_event(m.sensor, m.type, m.severity, "{}");
}

static void queue_sensor_event(const char *sensor, const char *type, const char *severity)
{
    if (!s_event_q) { api_send_event(sensor, type, severity, "{}"); return; }
    evt_send_t m;
    strlcpy(m.sensor,   sensor,   sizeof(m.sensor));
    strlcpy(m.type,     type,     sizeof(m.type));
    strlcpy(m.severity, severity, sizeof(m.severity));
    if (xQueueSend(s_event_q, &m, 0) != pdTRUE)
        ESP_LOGW(TAG, "event send queue full — dropping %s from %s", type, sensor);
}

/* ── EUI64 helpers ────────────────────────────────────────────────────────── */
static void eui64_to_hex(const uint8_t eui64[8], char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", eui64[i]);
}

/* True only if this EUI64 is in the Thread sensor table and its enabled flag is set. */
static bool thread_sensor_forwardable(const uint8_t eui64[8])
{
    uint8_t eui64s[10][8];
    bool    enabled[10];
    int count = nvs_load_thread_sensors(eui64s, NULL, NULL, enabled, 10);
    for (int i = 0; i < count; i++)
        if (memcmp(eui64s[i], eui64, 8) == 0) return enabled[i];
    return false;
}

/* Resolve an EUI64 that may be a Thread MLE *extended address* back to the canonical factory EUI64
 * stored in NVS. The child-table liveness events (SENSOR_LOST/ONLINE) carry the child's MLE ext
 * address, which per Thread spec is the factory EUI64 with the U/L bit (0x02 of byte 0) inverted —
 * e.g. NVS "f4ce.." shows up in the child table as "f6ce..". SENSOR_DATA is unaffected (the sensor
 * self-reports its factory EUI64 in JSON), so only these two events need normalizing. Match with
 * that one bit masked off (direction-agnostic) and hand back the stored factory EUI64 so the
 * watchdog, DATA path, and TFT badge all key on the same value. Returns false if unknown. */
static bool thread_resolve_eui64(const uint8_t in[8], uint8_t out[8])
{
    uint8_t eui64s[10][8];
    int count = nvs_load_thread_sensors(eui64s, NULL, NULL, NULL, 10);
    for (int i = 0; i < count; i++) {
        if (((eui64s[i][0] & ~0x02) == (in[0] & ~0x02)) &&
            memcmp(&eui64s[i][1], &in[1], 7) == 0) {
            memcpy(out, eui64s[i], 8);
            return true;
        }
    }
    return false;
}

/* ── Per-sensor liveness watchdog (snapshot-authoritative, pull-based) ─────────
 * A sensor is ALIVE iff it is enabled (toggled on) AND present in the nRF's Thread child table.
 * The child table is the authoritative source: the hub PULLS a snapshot (CMD_CHILD_POLL → the nRF
 * replies IPC_EVT_CHILD_LIST) on a slow heartbeat, on a toggle, and after a join — and reconciles.
 * A sensor absent from WD_MISS_LIMIT consecutive snapshots is marked WD_DEAD ("not in mesh") →
 * app notification + TFT offline badge; it auto-clears the moment it reappears. Events are NOT the
 * liveness signal (these sensors fire on window break/open, possibly hours apart) — only mesh
 * membership is. "Dead" is a soft status: the sensor stays paired/enabled in NVS.
 *
 * This deliberately replaces the old local-timer model (WD_OFFLINE_MS on a last_seen refreshed by
 * the child poll): that raced an unreliable push and declared healthy, actively-reporting sensors
 * dead 8 s after each sparse event. */
typedef enum { WD_ONLINE = 0, WD_DEAD } wd_state_t;
typedef struct {
    uint8_t    eui64[8];
    bool       used;
    wd_state_t state;
    int        miss;          /* consecutive child-table snapshots this enabled sensor was absent from */
} wd_entry_t;

#define WD_MAX          10
#define WD_POLL_MS      30000  /* heartbeat: pull the child table this often (events are rare; latency isn't critical) */
#define WD_START_MS     25000  /* grace after Thread net-up before the first poll — let sensors (re)attach */
#define WD_MISS_LIMIT   4      /* consecutive snapshots absent before declaring a sensor not-in-mesh (debounce) */

static wd_entry_t        s_wd[WD_MAX];
static SemaphoreHandle_t s_wd_mutex;

static wd_entry_t *wd_find(const uint8_t eui64[8])
{
    for (int i = 0; i < WD_MAX; i++)
        if (s_wd[i].used && memcmp(s_wd[i].eui64, eui64, 8) == 0) return &s_wd[i];
    return NULL;
}

static wd_entry_t *wd_get_or_add(const uint8_t eui64[8])
{
    wd_entry_t *e = wd_find(eui64);
    if (e) return e;
    for (int i = 0; i < WD_MAX; i++) {
        if (!s_wd[i].used) {
            s_wd[i].used  = true;
            memcpy(s_wd[i].eui64, eui64, 8);
            s_wd[i].state = WD_ONLINE;   /* optimistic — proven or disproven by the first snapshot */
            s_wd[i].miss  = 0;
            return &s_wd[i];
        }
    }
    return NULL;
}

/* A sensor is alive (a fast-path proof: it just joined, sent a forwardable event, or the nRF's
 * neighbor callback reported it (re)attached). Clear any miss streak, and if it had been DEAD,
 * transition it back ONLINE + notify the app + clear the TFT badge. The authoritative membership
 * check is still the child-table snapshot; this just makes recovery instant. */
static void wd_mark_online(const uint8_t eui64[8])
{
    if (!s_wd_mutex) return;
    bool was_dead = false;
    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
    wd_entry_t *e = wd_get_or_add(eui64);
    if (e) {
        e->miss = 0;
        if (e->state == WD_DEAD) {
            was_dead   = true;
            e->state   = WD_ONLINE;
        }
    }
    xSemaphoreGive(s_wd_mutex);
    if (was_dead) {
        char hex[17];
        eui64_to_hex(eui64, hex);
        ESP_LOGI(TAG, "watchdog: %s back in mesh", hex);
        queue_sensor_event(hex, "sensor_online", "info");
        display_set_thread_sensor_offline(eui64, false);
    }
}

bool nrf_thread_is_sensor_offline(const uint8_t eui64[8])
{
    if (!s_wd_mutex) return false;
    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
    wd_entry_t *e = wd_find(eui64);
    bool off = (e && e->state == WD_DEAD);
    xSemaphoreGive(s_wd_mutex);
    return off;
}

/* Drop watchdog entries for sensors no longer enabled in NVS (toggled off or deleted) — a disabled
 * sensor must never alert, so clear any offline badge too. Enabled sensors get their monitored
 * entry lazily in wd_reconcile_snapshot(). Called each heartbeat, so toggles/deletes take effect
 * within one poll cycle. */
static void wd_sync_from_nvs(void)
{
    uint8_t eui64s[10][8];
    bool    enabled[10];
    int count = nvs_load_thread_sensors(eui64s, NULL, NULL, enabled, 10);

    uint8_t cleared[WD_MAX][8];
    int     n_cleared = 0;

    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
    for (int j = 0; j < WD_MAX; j++) {
        wd_entry_t *e = &s_wd[j];
        if (!e->used) continue;
        bool still = false;
        for (int i = 0; i < count; i++)
            if (enabled[i] && memcmp(eui64s[i], e->eui64, 8) == 0) { still = true; break; }
        if (!still) {
            memcpy(cleared[n_cleared++], e->eui64, 8);
            e->used = false;
        }
    }
    xSemaphoreGive(s_wd_mutex);

    for (int k = 0; k < n_cleared; k++)
        display_set_thread_sensor_offline(cleared[k], false);
}

/* Authoritative reconcile against a fresh child-table snapshot (the factory EUI64s the nRF reports
 * as present). For every ENABLED sensor: present ⇒ ONLINE (instant recovery if it had been DEAD);
 * absent ⇒ bump its miss streak, and only once it has been absent from WD_MISS_LIMIT consecutive
 * snapshots do we declare it "not in mesh" (WD_DEAD) → app notification + TFT badge + one reconnect
 * nudge. A present sensor is NEVER reconnect-commissioned. Notifications/reconnects are collected
 * under the lock and issued after releasing it (they block on HTTPS / UART). */
static void wd_reconcile_snapshot(const uint8_t present[][8], int n_present)
{
    if (!s_wd_mutex) return;

    uint8_t eui64s[10][8];
    bool    enabled[10];
    int count = nvs_load_thread_sensors(eui64s, NULL, NULL, enabled, 10);

    uint8_t online_eui[WD_MAX][8];  int n_online = 0;   /* DEAD → ONLINE this pass */
    uint8_t dead_eui[WD_MAX][8];    int n_dead   = 0;   /* ONLINE → DEAD this pass */
    uint8_t reconnect_eui[WD_MAX][8]; int n_reconnect = 0;

    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        if (!enabled[i]) continue;
        wd_entry_t *e = wd_get_or_add(eui64s[i]);
        if (!e) continue;

        bool is_present = false;
        for (int p = 0; p < n_present; p++)
            if (memcmp(present[p], eui64s[i], 8) == 0) { is_present = true; break; }

        if (is_present) {
            e->miss = 0;
            if (e->state == WD_DEAD) {
                e->state = WD_ONLINE;
                memcpy(online_eui[n_online++], e->eui64, 8);
            }
        } else {
            if (e->miss < 1000000) e->miss++;
            if (e->state == WD_ONLINE && e->miss >= WD_MISS_LIMIT) {
                e->state = WD_DEAD;
                memcpy(dead_eui[n_dead++], e->eui64, 8);
                memcpy(reconnect_eui[n_reconnect++], e->eui64, 8);
            } else if (e->state == WD_DEAD) {
                /* still absent — keep nudging it to rejoin */
                memcpy(reconnect_eui[n_reconnect++], e->eui64, 8);
            }
        }
    }

    /* Summary snapshot (printed on change, below). */
    int connected = 0, in_mesh = 0, missing = 0;
    for (int j = 0; j < WD_MAX; j++) {
        if (!s_wd[j].used) continue;
        connected++;
        if (s_wd[j].state == WD_ONLINE) in_mesh++; else missing++;
    }
    xSemaphoreGive(s_wd_mutex);

    for (int k = 0; k < n_online; k++) {
        char hex[17]; eui64_to_hex(online_eui[k], hex);
        ESP_LOGI(TAG, "watchdog: %s back in mesh", hex);
        queue_sensor_event(hex, "sensor_online", "info");
        display_set_thread_sensor_offline(online_eui[k], false);
    }
    for (int k = 0; k < n_dead; k++) {
        char hex[17]; eui64_to_hex(dead_eui[k], hex);
        ESP_LOGW(TAG, "sensor %s not in mesh — reporting offline", hex);
        queue_sensor_event(hex, "sensor_offline", "critical");
        display_set_thread_sensor_offline(dead_eui[k], true);
    }
    for (int k = 0; k < n_reconnect; k++)
        nrf_thread_reconnect_sensor(reconnect_eui[k]);

    static int pc = -1, pm = -1, px = -1;   /* -1 seeds a first print */
    if (connected != pc || in_mesh != pm || missing != px) {
        pc = connected; pm = in_mesh; px = missing;
        ESP_LOGI(TAG, "sensors: %d connected, %d in mesh, %d not in mesh",
                 connected, in_mesh, missing);
    }
}

/* Pull-based liveness: once Thread is up (plus a grace window for sensors to attach), request an
 * authoritative child-table snapshot every WD_POLL_MS. The reply (IPC_EVT_CHILD_LIST) drives
 * wd_reconcile_snapshot on the IPC worker. Toggles and joins request an extra immediate poll. */
static void watchdog_task(void *arg)
{
    while (!g_thread_net_ready) vTaskDelay(pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(WD_START_MS));   /* let enabled sensors (re)attach before the first check */

    for (;;) {
        wd_sync_from_nvs();      /* drop disabled/deleted before we ask */
        ipc_cmd_child_poll();    /* nRF replies with IPC_EVT_CHILD_LIST → wd_reconcile_snapshot */
        vTaskDelay(pdMS_TO_TICKS(WD_POLL_MS));
    }
}

/* ── IPC event handler ────────────────────────────────────────────────────── */
static void on_ipc_event(uint8_t type, const uint8_t *payload, uint16_t len)
{
    switch (type) {

    case IPC_EVT_PONG:
        if (!s_ipc_alive) {
            s_ipc_alive = true;
            ESP_LOGI(TAG, "UART handshake OK — nRF is alive");
        }
        break;

    case IPC_EVT_NET_UP: {
        if (len < 3) break;
        uint8_t  channel = payload[0];
        uint16_t panid   = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
        /* The nRF re-sends NET_UP on every reconnect (commissioner reopen); only log the first. */
        if (!g_thread_net_ready)
            ESP_LOGI(TAG, "Thread net up: channel=%u PAN=0x%04x", channel, panid);
        g_thread_net_ready = true;
        break;
    }

    case IPC_EVT_NET_DOWN:
        ESP_LOGW(TAG, "Thread net down");
        g_thread_net_ready = false;
        break;

    case IPC_EVT_SENSOR_JOINED: {
        if (len < 8) break;
        char hex[17];
        eui64_to_hex(payload, hex);
        ESP_LOGI(TAG, "Sensor joined Thread: eui64=%s", hex);

        /* Load existing Thread sensors, append new one, save back */
        uint8_t eui64s[10][8];
        char    names[10][32];
        char    zones[10][32];
        bool    enabled[10];
        int     count = nvs_load_thread_sensors(eui64s, names, zones, enabled, 10);

        /* Skip if already stored */
        for (int i = 0; i < count; i++) {
            if (memcmp(eui64s[i], payload, 8) == 0) {
                ESP_LOGI(TAG, "Sensor eui64=%s already in NVS", hex);
                goto joined_confirm;
            }
        }

        if (count < 10) {
            memcpy(eui64s[count], payload, 8);
            snprintf(names[count], sizeof(names[count]), "Sensor-%s", hex + 8);
            zones[count][0] = '\0';
            enabled[count] = true;      /* newly paired sensors start enabled */
            count++;
            nvs_save_thread_sensors(eui64s, names, zones, enabled, count);
        } else {
            ESP_LOGW(TAG, "Thread sensor table full (10), dropping %s", hex);
        }

joined_confirm:
        /* Notify server — reuse api_confirm_sensor with hex EUI64 as identifier.
         * Server-side schema update (eui64 vs MAC) is tracked separately. */
        api_confirm_sensor(hex);
        wd_mark_online(payload);   /* a fresh join counts as alive */
        ipc_cmd_child_poll();      /* confirm the new child against an authoritative snapshot */
        break;
    }

    case IPC_EVT_SENSOR_DATA: {
        if (len < 9) break;          /* need at least eui64 + 1 byte JSON */
        if (len >= 256) len = 255;
        ((uint8_t *)payload)[len] = '\0';
        char hex[17];
        eui64_to_hex(payload, hex);

        /* JSON payload starts after the 8-byte EUI64 */
        const char *json = (const char *)(payload + 8);
        ESP_LOGI(TAG, "Sensor data from %s: %s", hex, json);

        /* Gate: forward only for sensors that are in the table AND enabled. A deleted sensor
         * (removed from NVS) or a disabled one is silenced here — the hub is the chokepoint to
         * the server, so gating here is authoritative regardless of Thread-layer join state. */
        if (!thread_sensor_forwardable(payload)) {
            ESP_LOGW(TAG, "event from %s dropped (unknown or disabled)", hex);
            break;
        }

        wd_mark_online(payload);   /* any forwardable event proves the sensor is alive */

        /* Parse {"e":"<event_name>"} and forward to cloud */
        char event_buf[32] = {0};
        const char *e_start = strstr(json, "\"e\":\"");
        if (e_start) {
            e_start += 5;
            const char *e_end = strchr(e_start, '"');
            if (e_end && (e_end - e_start) < (int)sizeof(event_buf)) {
                memcpy(event_buf, e_start, e_end - e_start);
                event_buf[e_end - e_start] = '\0';
            }
        }

        if (event_buf[0]) {
            /* Map sensor event names to the server's canonical event types. "vibration" becomes
             * "shock_detected" — the server already treats that as critical, FCM-pushes it, and
             * titles the notification "Shock detected" (its text literally reads "vibration sensor
             * detected shock"), so no server change is needed. */
            const char *event_type = event_buf;
            if (strcmp(event_buf, "door_open") == 0)       event_type = "door_opened";
            else if (strcmp(event_buf, "door_close") == 0) event_type = "door_closed";
            else if (strcmp(event_buf, "vibration") == 0)  event_type = "shock_detected";

            /* Severe events the server elevates + pushes; everything else is informational. */
            const char *severity =
                (strcmp(event_type, "shock_detected") == 0 ||
                 strcmp(event_type, "door_opened") == 0) ? "critical" : "info";

            queue_sensor_event(hex, event_type, severity);   /* off the IPC worker — never block liveness */
        } else {
            ESP_LOGW(TAG, "Could not parse event from: %s", json);
        }
        break;
    }

    case IPC_EVT_COMM_FAILED: {
        if (len < 8) break;
        char hex[17];
        eui64_to_hex(payload, hex);
        ESP_LOGW(TAG, "Thread commissioning failed for eui64=%s", hex);
        break;
    }

    case IPC_EVT_SENSOR_LOST: {
        if (len < 8) break;
        /* The nRF's neighbor callback is a hint, not proof (it proved unreliable — see the nRF
         * comment). Don't declare the sensor dead from it; instead pull an authoritative child-table
         * snapshot to confirm. wd_reconcile_snapshot then does the debounced ONLINE→DEAD decision. */
        ipc_cmd_child_poll();
        break;
    }

    case IPC_EVT_SENSOR_ONLINE: {
        if (len < 8) break;
        /* A (re)attach hint — clear DEAD instantly; the next snapshot confirms it authoritatively. */
        uint8_t eui[8];
        if (thread_resolve_eui64(payload, eui)) wd_mark_online(eui);
        break;
    }

    case IPC_EVT_CHILD_LIST: {
        /* Authoritative liveness snapshot: N × child MLE ext-address. Resolve each back to its
         * factory EUI64 and reconcile the enabled-sensor set against it (present ⇒ in mesh; absent
         * for WD_MISS_LIMIT consecutive snapshots ⇒ not in mesh). This is the ONLY place a sensor is
         * declared in/out of the mesh. */
        int n = len / 8;
        uint8_t present[WD_MAX][8];
        int n_present = 0;
        char logbuf[WD_MAX * 17 + 8] = {0};
        int pos = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *ext = payload + i * 8;
            uint8_t eui[8];
            if (thread_resolve_eui64(ext, eui) && n_present < WD_MAX) {
                memcpy(present[n_present++], eui, 8);
                char hx[17];
                eui64_to_hex(eui, hx);
                if (pos < (int)sizeof(logbuf) - 18)
                    pos += snprintf(logbuf + pos, sizeof(logbuf) - pos, "%s ", hx);
            }
        }
        /* Log only when the present set changes, so the steady state stays quiet. */
        static int     s_last_n   = -1;
        static uint8_t s_last_first[8];
        bool changed = (n_present != s_last_n) ||
                       (n_present > 0 && memcmp(s_last_first, present[0], 8) != 0);
        if (changed) {
            s_last_n = n_present;
            if (n_present > 0) memcpy(s_last_first, present[0], 8);
            ESP_LOGI(TAG, "child table: %d in mesh: %s", n_present, n_present ? logbuf : "(none)");
        }
        wd_reconcile_snapshot(present, n_present);
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown IPC event 0x%02x", type);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Boot handshake: wait for the nRF to finish booting (its UART is not ready until ~3.8 s),
 * then a short fixed burst of PINGs, then stop. Starting at ~4 s means all 5 pings hit a
 * listening nRF (5 PONG chances, not ~2). The first PONG latches s_ipc_alive (logged once in
 * on_ipc_event). No keepalive, no reprobe: after the burst the hub goes silent. */
#define IPC_HANDSHAKE_START_MS 2500   /* task starts ~1.6 s after boot; +2.5 s => 1st ping ~4 s */
#define IPC_HANDSHAKE_PINGS    5
#define IPC_HANDSHAKE_GAP_MS   1000

static void ipc_handshake_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(IPC_HANDSHAKE_START_MS));   /* let the nRF finish booting */
    for (int i = 1; i <= IPC_HANDSHAKE_PINGS; i++) {
        ipc_cmd_ping();
        ESP_LOGI(TAG, "handshake PING %d/%d", i, IPC_HANDSHAKE_PINGS);
        vTaskDelay(pdMS_TO_TICKS(IPC_HANDSHAKE_GAP_MS));
    }
    if (s_ipc_alive) {
        ESP_LOGI(TAG, "nRF handshake complete — link up");
    } else {
        ESP_LOGW(TAG, "nRF handshake done — no PONG (check nRF power/wiring)");
    }
    vTaskDelete(NULL);
}

/* The nRF forms its Thread network autonomously at its own boot (a self-contained 802.15.4
 * network, independent of WiFi and the hub). The hub does NOT drive formation — it only learns
 * the state: once the UART link is up, poll CMD_NET_STATUS until the nRF reports NET_UP. This is
 * a read-only query (never changes nRF state, unlike NET_FORM), so it's safe to repeat — and it
 * also covers a hub-only reset while the nRF is already up (no role change → no async NET_UP),
 * plus a NET_UP frame lost to EMI. */
#define NET_STATUS_POLL_MS   3000
#define NET_STATUS_MAX_TRIES 10

static void net_watch_task(void *arg)
{
    while (!s_ipc_alive) vTaskDelay(pdMS_TO_TICKS(100));

    for (int i = 0; i < NET_STATUS_MAX_TRIES && !g_thread_net_ready; i++) {
        ipc_cmd_net_status();
        for (int t = 0; t < NET_STATUS_POLL_MS / 100 && !g_thread_net_ready; t++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (g_thread_net_ready)
        ESP_LOGI(TAG, "Thread network confirmed up");
    else
        ESP_LOGW(TAG, "Thread network not up after %d s of status polls — check nRF",
                 (NET_STATUS_POLL_MS * NET_STATUS_MAX_TRIES) / 1000);
    vTaskDelete(NULL);
}

void nrf_thread_preinit(void)
{
    nrf_ipc_init(on_ipc_event);
    ESP_LOGI(TAG, "nRF IPC UART ready at boot (RX=GPIO%d TX=GPIO%d)", NRF_UART_RX_GPIO, NRF_UART_TX_GPIO);
    /* Fire the handshake burst at boot, decoupled from WiFi. */
    xTaskCreate(ipc_handshake_task, "ipc_hs", 3072, NULL, 3, NULL);
    /* Passive watcher: confirms the nRF's autonomous Thread network came up. */
    xTaskCreate(net_watch_task, "net_watch", 3072, NULL, 3, NULL);
    /* Cloud-event send lane: keeps the blocking HTTPS POST off the IPC worker. */
    s_event_q = xQueueCreate(8, sizeof(evt_send_t));
    xTaskCreate(event_sender_task, "evt_send", 6144, NULL, 4, NULL);
    /* Per-sensor liveness watchdog: pull child-table snapshots + notify when a sensor leaves the mesh. */
    s_wd_mutex = xSemaphoreCreateMutex();
    xTaskCreate(watchdog_task, "sensor_wd", 6144, NULL, 3, NULL);
}

void nrf_thread_on_wifi_ready(void)
{
    /* Thread formation is autonomous on the nRF and independent of WiFi — nothing to do here. */
    ESP_LOGI(TAG, "WiFi ready (Thread network is formed autonomously by the nRF)");
}

void nrf_thread_commission_sensor(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s)
{
    ipc_cmd_commission(eui64, pskd, timeout_s);
}

void nrf_thread_delete_sensor(const uint8_t eui64[8])
{
    /* Tell the nRF to drop the joiner and multicast a "leave" so the sensor factory-resets. */
    ipc_cmd_sensor_del(eui64);

    /* Remove from NVS (compacts eui64/name/zone/enabled together, keeping them aligned). */
    uint8_t eui64s[10][8];
    char    names[10][32];
    char    zones[10][32];
    bool    enabled[10];
    int     count = nvs_load_thread_sensors(eui64s, names, zones, enabled, 10);
    int     new_count = 0;
    for (int i = 0; i < count; i++) {
        if (memcmp(eui64s[i], eui64, 8) != 0) {
            if (new_count != i) {
                memcpy(eui64s[new_count], eui64s[i], 8);
                memcpy(names[new_count], names[i], 32);
                memcpy(zones[new_count], zones[i], 32);
                enabled[new_count] = enabled[i];
            }
            new_count++;
        }
    }
    nvs_save_thread_sensors(eui64s, names, zones, enabled, new_count);
}

void nrf_thread_reconnect_sensor(const uint8_t eui64[8])
{
    /* PSKd = last 4 bytes of EUI64 as uppercase hex — must match the sensor's derive_pskd()
     * and the pairing path (sensor_pairing.c). Reopen the commissioner for 60 s so a sensor
     * that is retrying the Joiner can rejoin. */
    char pskd[9];
    snprintf(pskd, sizeof(pskd), "%02X%02X%02X%02X",
             eui64[4], eui64[5], eui64[6], eui64[7]);
    char hex[17];
    eui64_to_hex(eui64, hex);
    ESP_LOGD(TAG, "Thread sensor %s reconnect nudge (pskd=%s, 60s window)", hex, pskd);
    ipc_cmd_commission(eui64, pskd, 60);
}

void nrf_thread_set_sensor_enabled(const uint8_t eui64[8], bool en)
{
    uint8_t eui64s[10][8];
    char    names[10][32];
    char    zones[10][32];
    bool    enabled[10];
    int     count = nvs_load_thread_sensors(eui64s, names, zones, enabled, 10);
    for (int i = 0; i < count; i++) {
        if (memcmp(eui64s[i], eui64, 8) == 0) {
            enabled[i] = en;
            nvs_save_thread_sensors(eui64s, names, zones, enabled, count);
            char hex[17];
            eui64_to_hex(eui64, hex);
            ESP_LOGI(TAG, "Thread sensor %s %s", hex, en ? "enabled" : "disabled");
            /* Enabling also nudges a reconnect so a dropped sensor can rejoin. */
            if (en) nrf_thread_reconnect_sensor(eui64);
            /* Re-check membership immediately: a disable must clear any offline badge, an enable must
             * (re)evaluate the sensor against a fresh snapshot rather than waiting a full poll cycle. */
            ipc_cmd_child_poll();
            return;
        }
    }
}
