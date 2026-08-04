#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ── Main namespace ("glazia") — WiFi credentials + hub identity ───────────
void nvs_save_credentials(void);
bool nvs_load_credentials(void);
void nvs_clear_credentials(void);

// Persist one enrolled master fingerprint. The template lives in R307 flash;
// NVS only tracks metadata needed by hub firmware.
void nvs_save_fingerprint(bool enrolled, const char *fingerprint_id, uint16_t slot);
bool nvs_load_fingerprint(bool *out_enrolled, char *out_id, size_t out_id_len, uint16_t *out_slot);
bool nvs_load_fp_enrolled(void);
void nvs_save_fingerprints(uint8_t count, const uint16_t *slots);
uint8_t nvs_load_fingerprints(uint16_t *slots, uint8_t max_slots);

// Persist paired sensor MAC + LMK list (call after each successful ESP-NOW ACK)
// keys[i] is a 16-byte raw LMK that matches the sensor's provision_key
void nvs_save_sensors(const char macs[][18], const uint8_t keys[][16],
                      const char names[][32], const char zones[][32], int count);

// Load saved sensors — returns count loaded (0 if none).
// keys may be NULL if the caller only needs MACs.
int  nvs_load_sensors(char macs[][18], uint8_t keys[][16],
                      char names[][32], char zones[][32], int max_count);
void nvs_save_sensor_enabled(int index, bool enabled);
bool nvs_load_sensor_enabled(int index, bool default_enabled);

// ── Provisional namespace ("glazia_prov") — pending sensor during pairing ─
// Written by the button polling task when api_fetch_sensor_pairing() succeeds.
// Cleared either on successful ACK (after promoting to main) or on timeout.
void nvs_prov_save_sensor(const char *sensor_mac, const char *provision_key_hex,
                          const char *name, const char *zone);
bool nvs_prov_load_sensor(char *out_sensor_mac, char *out_provision_key_hex,
                          char *out_name, size_t out_name_len,
                          char *out_zone, size_t out_zone_len);
void nvs_prov_clear(void);
