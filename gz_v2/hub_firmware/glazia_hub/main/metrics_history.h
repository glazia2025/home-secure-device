#ifndef GLAZIA_METRICS_HISTORY_H
#define GLAZIA_METRICS_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rolling 24-hour history of temperature + humidity, used by the dashboard for
 * the MIN/AVG/MAX-24H stat cells and the humidity wave chart. Backed by a small
 * fixed ring of 30-minute buckets (48 buckets = 24 h); no dynamic allocation.
 * Thread-safe: push() runs on the sensor task, stats()/series() on the LVGL
 * task, guarded by an internal mutex. */

typedef enum {
    METRIC_TEMP = 0,
    METRIC_HUM  = 1,
} metric_kind_t;

/* Number of history points exposed for the wave chart (one per 30-min bucket). */
#define METRICS_HISTORY_POINTS 48

/* Record a new reading (called once per real sensor update). */
void metrics_history_push(float temp, float hum);

/* Fill the min, avg and max outputs over the last 24 h. Returns false (and
 * leaves outputs untouched) if no samples have been recorded yet. */
bool metrics_history_stats(metric_kind_t kind, float *out_min, float *out_avg, float *out_max);

/* Fill out[0..n-1] with the most-recent bucket averages, oldest → newest.
 * Empty buckets carry forward the previous known value so the chart has no
 * gaps. Returns the number of leading points that are real data (the caller can
 * treat the buffer as fully populated once >0). */
size_t metrics_history_series(metric_kind_t kind, float *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* GLAZIA_METRICS_HISTORY_H */
