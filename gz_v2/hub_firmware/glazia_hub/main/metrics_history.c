#include "metrics_history.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#include <string.h>

/* 48 × 30-minute buckets = a rolling 24-hour window. Each bucket accumulates
 * count/sum/min/max so we can report MIN/AVG/MAX cheaply and expose per-bucket
 * averages for the wave chart. Buckets are keyed by an absolute index derived
 * from the monotonic esp_timer clock, so the window is independent of NTP. */
#define NB           METRICS_HISTORY_POINTS
#define BUCKET_US    (30LL * 60 * 1000000)   /* 30 minutes */

typedef struct {
    uint32_t n;
    float    sum;
    float    mn;
    float    mx;
} bucket_t;

static bucket_t s_temp[NB];
static bucket_t s_hum[NB];
static int64_t  s_last_abs = -1;             /* absolute bucket index of newest sample */
static SemaphoreHandle_t s_mux;

static void ensure_mux(void)
{
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
}

static void bucket_reset(bucket_t *b)
{
    b->n = 0;
    b->sum = 0.0f;
    b->mn = 0.0f;
    b->mx = 0.0f;
}

static void bucket_add(bucket_t *b, float v)
{
    if (b->n == 0) {
        b->mn = v;
        b->mx = v;
    } else {
        if (v < b->mn) b->mn = v;
        if (v > b->mx) b->mx = v;
    }
    b->sum += v;
    b->n++;
}

/* Advance to absolute bucket `abs`, clearing any slots newly entered since the
 * last sample (so stale >24 h data never lingers in the ring). */
static void advance_locked(int64_t abs)
{
    if (s_last_abs < 0) {
        s_last_abs = abs;
        bucket_reset(&s_temp[abs % NB]);
        bucket_reset(&s_hum[abs % NB]);
        return;
    }
    if (abs <= s_last_abs) return;

    int64_t steps = abs - s_last_abs;
    if (steps > NB) steps = NB;
    for (int64_t i = 1; i <= steps; i++) {
        int slot = (int)((s_last_abs + i) % NB);
        bucket_reset(&s_temp[slot]);
        bucket_reset(&s_hum[slot]);
    }
    s_last_abs = abs;
}

void metrics_history_push(float temp, float hum)
{
    ensure_mux();
    if (!s_mux) return;
    int64_t abs = esp_timer_get_time() / BUCKET_US;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    advance_locked(abs);
    int slot = (int)(abs % NB);
    bucket_add(&s_temp[slot], temp);
    bucket_add(&s_hum[slot], hum);
    xSemaphoreGive(s_mux);
}

bool metrics_history_stats(metric_kind_t kind, float *out_min, float *out_avg, float *out_max)
{
    ensure_mux();
    if (!s_mux) return false;

    const bucket_t *buckets = (kind == METRIC_HUM) ? s_hum : s_temp;
    bool have = false;
    float mn = 0, mx = 0, sum = 0;
    uint32_t total = 0;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int i = 0; i < NB; i++) {
        if (buckets[i].n == 0) continue;
        if (!have) {
            mn = buckets[i].mn;
            mx = buckets[i].mx;
            have = true;
        } else {
            if (buckets[i].mn < mn) mn = buckets[i].mn;
            if (buckets[i].mx > mx) mx = buckets[i].mx;
        }
        sum += buckets[i].sum;
        total += buckets[i].n;
    }
    xSemaphoreGive(s_mux);

    if (!have || total == 0) return false;
    if (out_min) *out_min = mn;
    if (out_max) *out_max = mx;
    if (out_avg) *out_avg = sum / (float)total;
    return true;
}

size_t metrics_history_series(metric_kind_t kind, float *out, size_t n)
{
    ensure_mux();
    if (!s_mux || !out || n == 0) return 0;
    if (n > NB) n = NB;

    const bucket_t *buckets = (kind == METRIC_HUM) ? s_hum : s_temp;
    size_t real = 0;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    int64_t newest = s_last_abs;
    if (newest < 0) {
        xSemaphoreGive(s_mux);
        return 0;
    }
    /* Walk the last n buckets oldest → newest, carrying forward the last known
     * value across empty buckets so the chart line stays continuous. */
    float last = 0.0f;
    bool seeded = false;
    for (size_t i = 0; i < n; i++) {
        int64_t abs = newest - (int64_t)(n - 1 - i);
        float v = last;
        if (abs >= 0) {
            const bucket_t *b = &buckets[abs % NB];
            if (b->n > 0) {
                v = b->sum / (float)b->n;
                last = v;
                seeded = true;
                real++;
            }
        }
        out[i] = seeded ? v : 0.0f;
    }
    xSemaphoreGive(s_mux);
    return real;
}
