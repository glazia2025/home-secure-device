#pragma once
#include <stdbool.h>
#include <stdint.h>

// ── Main namespace ("glazia") — WiFi credentials + hub identity ───────────
void nvs_save_credentials(void);
bool nvs_load_credentials(void);
void nvs_clear_credentials(void);

// Persist paired sensor MAC + LMK list (call after each successful ESP-NOW ACK)
// keys[i] is a 16-byte raw LMK that matches the sensor's provision_key
void nvs_save_sensors(const char macs[][18], const uint8_t keys[][16], int count);

// Load saved sensors — returns count loaded (0 if none).
// keys may be NULL if the caller only needs MACs.
int  nvs_load_sensors(char macs[][18], uint8_t keys[][16], int max_count);

// ── Provisional namespace ("glazia_prov") — pending sensor during pairing ─
// Written by the button polling task when api_fetch_sensor_pairing() succeeds.
// Cleared either on successful ACK (after promoting to main) or on timeout.
void nvs_prov_save_sensor(const char *sensor_mac, const char *provision_key_hex);
bool nvs_prov_load_sensor(char *out_sensor_mac, char *out_provision_key_hex);
void nvs_prov_clear(void);
