#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Initialise ESP-NOW and register callbacks.
// Sets the global PMK so encrypted peers can be added.
void espnow_init(void);

// Stop ESP-NOW for fingerprint-gated hub offline mode.
void espnow_deinit(void);

// Begin pairing a new sensor using the provision_key as its ESP-NOW LMK.
// provision_key_hex: 32-char hex string (16 bytes), e.g. "a1b2c3...".
// On validated ACK: commits hub NVS and sends COMMIT so the sensor can commit.
// On timeout: clears provisional NVS.
void espnow_pair_sensor(const char *sensor_mac_str, const char *provision_key_hex,
                        const char *name, const char *zone);

// On reboot: load saved sensor MACs + LMK keys from NVS and re-add each as an
// encrypted ESP-NOW peer. Does NOT send HELLO (sensors reconnect on their own).
void espnow_reconnect_saved_sensors(void);

// Fill `out` with a semicolon-separated sensor list for the display:
//   "S1|Unknown|ON;S2|Unknown|OFF;..."
// Caller supplies the output buffer and its size.
void espnow_get_sensor_list_str(char *out, size_t out_len);

int espnow_get_sensor_count(void);
int espnow_get_active_sensor_count(void);
bool espnow_get_sensor_info(int index, char *out_name, size_t out_name_len, bool *out_enabled, bool *out_paired);
void espnow_set_sensor_enabled(int index, bool enabled);
esp_err_t espnow_set_sensor_enabled_by_mac(const char *mac_str, bool enabled);

// Remove a paired sensor by MAC string: sends PKT_RESET to sensor (best-effort),
// deletes ESP-NOW peer, and saves the updated sensor table to NVS.
// Returns ESP_ERR_NOT_FOUND if the MAC is not in the peer table.
esp_err_t espnow_remove_sensor(const char *sensor_mac_str);

// Send PKT_RESET to every currently-paired sensor peer (best-effort, no retry).
// Call before hub NVS erase/restart so sensors can clear their own NVS.
void espnow_send_reset_to_all_sensors(void);

// Queue a hub-level event (no sensor MAC) to be forwarded to the server.
// Safe to call from any task — non-blocking xQueueSend.
void espnow_queue_hub_event(const char *event_type);
