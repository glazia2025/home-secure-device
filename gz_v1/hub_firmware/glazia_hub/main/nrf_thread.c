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
#include <string.h>
#include <stdio.h>

static const char *TAG = "NRF_THR";

volatile bool g_thread_net_ready = false;
static volatile bool s_ipc_alive  = false;

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

/* ── Per-sensor liveness watchdog ─────────────────────────────────────────────
 * The nRF reports SENSOR_LOST when a sensor ages out of the Thread child table and
 * SENSOR_ONLINE when it (re)attaches (also cleared by any JOINED/forwardable DATA). A lost
 * sensor gets WD_RECONNECT_TRIES best-effort reconnect nudges spaced WD_RETRY_MS apart; if it
 * never returns it is marked WD_DEAD → app notification + TFT offline badge. "Dead" is a soft
 * status only: the sensor stays paired/enabled in NVS and auto-clears when it comes back. */
typedef enum { WD_ONLINE = 0, WD_RECOVERING, WD_DEAD } wd_state_t;
typedef struct {
    uint8_t    eui64[8];
    bool       used;
    bool       seen;          /* has been present in the child poll / sent data at least once */
    wd_state_t state;
    int        attempts_left;
    TickType_t next_attempt;
    TickType_t last_seen;     /* tick of the last child-poll presence or forwardable event */
} wd_entry_t;

#define WD_MAX             10
#define WD_RECONNECT_TRIES 3
#define WD_RETRY_MS        30000   /* spacing between the 3 reconnect nudges — give each attempt time */
#define WD_TICK_MS         2000    /* watchdog cadence — react promptly once a sensor drops */
#define WD_OFFLINE_MS      8000    /* absence from the child poll before declaring a sensor lost */
#define WD_HB_LOG_MS       10000   /* throttle for the "monitored sensors" liveness log */

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
            s_wd[i].used = true;
            s_wd[i].seen = false;
            memcpy(s_wd[i].eui64, eui64, 8);
            s_wd[i].state = WD_ONLINE;
            s_wd[i].attempts_left = 0;
            s_wd[i].next_attempt = 0;
            s_wd[i].last_seen = 0;
            return &s_wd[i];
        }
    }
    return NULL;
}

/* A sensor stopped responding — begin recovery. Only monitors known + enabled sensors. */
static void wd_mark_lost(const uint8_t eui64[8])
{
    if (!s_wd_mutex || !thread_sensor_forwardable(eui64)) return;
    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
    wd_entry_t *e = wd_get_or_add(eui64);
    if (e && e->state == WD_ONLINE) {
        e->state = WD_RECOVERING;
        e->attempts_left = WD_RECONNECT_TRIES;
        e->next_attempt = xTaskGetTickCount();
    }
    xSemaphoreGive(s_wd_mutex);
    char hex[17];
    eui64_to_hex(eui64, hex);
    ESP_LOGW(TAG, "sensor %s not in mesh — starting reconnect", hex);
}

/* A sensor is alive — refresh liveness (creating the monitored entry on first sight, e.g. from the
 * child poll). If it had been recovering/dead, clear that; if it was dead, notify + clear badge. */
static void wd_mark_online(const uint8_t eui64[8])
{
    if (!s_wd_mutex) return;
    bool was_dead = false, changed = false;
    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
    wd_entry_t *e = wd_get_or_add(eui64);
    if (e) {
        e->seen = true;
        e->last_seen = xTaskGetTickCount();
        if (e->state != WD_ONLINE) {
            was_dead = (e->state == WD_DEAD);
            e->state = WD_ONLINE;
            e->attempts_left = 0;
            changed = true;
        }
    }
    xSemaphoreGive(s_wd_mutex);
    if (changed) {
        char hex[17];
        eui64_to_hex(eui64, hex);
        ESP_LOGI(TAG, "watchdog: %s back online", hex);
        if (was_dead) {
            api_send_event(hex, "sensor_online", "info", "{}");
            display_set_thread_sensor_offline(eui64, false);
        }
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

/* Reconcile the watchdog table with the NVS enabled-sensor list. The authoritative "should be
 * online" set is the *enabled* Thread sensors (the toggle state, synced from the app) — NOT who has
 * appeared in the child table. So a sensor that is enabled but never attaches (dead from boot) is
 * still monitored, reconnected, and reported offline. Seeds an entry for every enabled sensor
 * (grace: last_seen = now on first sight, so a healthy sensor has time to (re)attach) and drops
 * entries for sensors now disabled or deleted (clearing any offline badge — a disabled sensor must
 * never alert). Called every watchdog tick, so toggles/deletes take effect within WD_TICK_MS. */
static void wd_sync_from_nvs(void)
{
    uint8_t eui64s[10][8];
    bool    enabled[10];
    int count = nvs_load_thread_sensors(eui64s, NULL, NULL, enabled, 10);

    uint8_t cleared[WD_MAX][8];
    int     n_cleared = 0;

    xSemaphoreTake(s_wd_mutex, portMAX_DELAY);

    /* Ensure a monitored entry for every enabled sensor. */
    for (int i = 0; i < count; i++) {
        if (!enabled[i]) continue;
        wd_entry_t *e = wd_get_or_add(eui64s[i]);
        if (e && !e->seen) {                 /* freshly created — begin its grace window now */
            e->seen = true;
            e->last_seen = xTaskGetTickCount();
        }
    }

    /* Drop entries no longer enabled in NVS (toggled off or deleted). */
    for (int j = 0; j < WD_MAX; j++) {
        wd_entry_t *e = &s_wd[j];
        if (!e->used) continue;
        bool still = false;
        for (int i = 0; i < count; i++)
            if (enabled[i] && memcmp(eui64s[i], e->eui64, 8) == 0) { still = true; break; }
        if (!still) {
            memcpy(cleared[n_cleared++], e->eui64, 8);
            e->used = false;
            e->seen = false;
        }
    }

    xSemaphoreGive(s_wd_mutex);

    for (int k = 0; k < n_cleared; k++)
        display_set_thread_sensor_offline(cleared[k], false);
}

static void watchdog_task(void *arg)
{
    TickType_t last_hb_log = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WD_TICK_MS));
        wd_sync_from_nvs();   /* monitor every enabled sensor, not just ones seen in the child table */
        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < WD_MAX; i++) {
            uint8_t eui[8];
            bool do_lost = false, do_reconnect = false, do_dead = false;
            int  attempt_no = 0;

            xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
            wd_entry_t *e = &s_wd[i];
            if (e->used) {
                memcpy(eui, e->eui64, 8);
                if (e->state == WD_ONLINE && e->seen &&
                    (int32_t)(now - e->last_seen) > (int32_t)pdMS_TO_TICKS(WD_OFFLINE_MS)) {
                    /* Gone from the child poll for too long — flag it (wd_mark_lost, below, does the
                     * enabled/known gate + the ONLINE→RECOVERING transition). */
                    do_lost = true;
                } else if (e->state == WD_RECOVERING &&
                           (int32_t)(now - e->next_attempt) >= 0) {
                    if (e->attempts_left > 0) {
                        do_reconnect = true;
                        e->attempts_left--;
                        attempt_no = WD_RECONNECT_TRIES - e->attempts_left;   /* 1..WD_RECONNECT_TRIES */
                        e->next_attempt = now + pdMS_TO_TICKS(WD_RETRY_MS);
                    } else {
                        do_dead = true;
                        e->state = WD_DEAD;
                    }
                }
            }
            xSemaphoreGive(s_wd_mutex);

            if (do_lost) wd_mark_lost(eui);   /* logs "not in mesh — starting reconnect" for enabled sensors */
            if (do_reconnect) {
                char hex[17];
                eui64_to_hex(eui, hex);
                ESP_LOGW(TAG, "sensor %s reconnect %d/%d", hex, attempt_no, WD_RECONNECT_TRIES);
                nrf_thread_reconnect_sensor(eui);
            }
            if (do_dead) {
                char hex[17];
                eui64_to_hex(eui, hex);
                ESP_LOGE(TAG, "sensor %s offline (reconnect failed) — notifying", hex);
                api_send_event(hex, "sensor_offline", "critical", "{}");
                display_set_thread_sensor_offline(eui, true);
            }
        }

        /* Throttled one-line summary: how many enabled sensors are in the mesh vs missing. */
        if ((int32_t)(now - last_hb_log) >= (int32_t)pdMS_TO_TICKS(WD_HB_LOG_MS)) {
            last_hb_log = now;
            int connected = 0, in_mesh = 0, missing = 0;
            xSemaphoreTake(s_wd_mutex, portMAX_DELAY);
            for (int i = 0; i < WD_MAX; i++) {
                wd_entry_t *e = &s_wd[i];
                if (!e->used) continue;
                connected++;
                if (e->state == WD_ONLINE) in_mesh++;
                else                       missing++;
            }
            xSemaphoreGive(s_wd_mutex);
            ESP_LOGI(TAG, "sensors: %d connected, %d in mesh, %d not in mesh",
                     connected, in_mesh, missing);
        }
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
        break;
    }

    case IPC_EVT_SENSOR_DATA: {
        if (len < 9) break;          /* need at least eui64 + 1 byte JSON */
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

            api_send_event(hex, event_type, severity, "{}");
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
        /* payload is the child's MLE ext address — normalize to the canonical factory EUI64. */
        uint8_t eui[8];
        if (thread_resolve_eui64(payload, eui)) wd_mark_lost(eui);
        break;
    }

    case IPC_EVT_SENSOR_ONLINE: {
        if (len < 8) break;
        uint8_t eui[8];
        if (thread_resolve_eui64(payload, eui)) wd_mark_online(eui);
        break;
    }

    case IPC_EVT_CHILD_LIST: {
        /* Authoritative liveness snapshot: N × child ext-address. Refresh every present + known
         * sensor's last_seen (the watchdog declares a sensor lost once it stops appearing here). */
        int n = len / 8;
        char present[WD_MAX * 17 + 8] = {0};
        int pos = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *ext = payload + i * 8;
            char hx[17];
            eui64_to_hex(ext, hx);
            if (pos < (int)sizeof(present) - 18)
                pos += snprintf(present + pos, sizeof(present) - pos, "%s ", hx);
            uint8_t eui[8];
            if (thread_resolve_eui64(ext, eui)) wd_mark_online(eui);
        }
        /* Poll runs every 3 s — only log when the present set actually changes (count + first eui),
         * so a steady state doesn't spam the serial. */
        static int     s_last_n   = -1;
        static uint8_t s_last_first[8];
        bool changed = (n != s_last_n) ||
                       (n > 0 && memcmp(s_last_first, payload, 8) != 0);
        if (changed) {
            s_last_n = n;
            if (n > 0) memcpy(s_last_first, payload, 8);
            ESP_LOGI(TAG, "child table: %d present: %s", n, n ? present : "(none)");
        }
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
    /* Per-sensor liveness watchdog: reconnect + notify when a sensor drops off the mesh. */
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
            return;
        }
    }
}
