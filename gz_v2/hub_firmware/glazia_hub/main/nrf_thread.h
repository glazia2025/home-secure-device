#pragma once
#include <stdint.h>
#include <stdbool.h>

extern volatile bool g_thread_net_ready;

void nrf_thread_preinit(void);
void nrf_thread_on_wifi_ready(void);
void nrf_thread_commission_sensor(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s);
void nrf_thread_delete_sensor(const uint8_t eui64[8]);
void nrf_thread_set_sensor_enabled(const uint8_t eui64[8], bool en);

/* Best-effort reconnect nudge: reopen the commissioner for this known sensor using its
 * EUI64-derived PSKd. No-op for an already-attached sensor; recovers one that lost its
 * credentials (fresh/factory-reset) and is retrying the Joiner. */
void nrf_thread_reconnect_sensor(const uint8_t eui64[8]);

/* True if the liveness watchdog has declared this sensor offline (dead). Used by the TFT to
 * render an offline badge on rebuild. Soft status only — the sensor stays paired. */
bool nrf_thread_is_sensor_offline(const uint8_t eui64[8]);
