#pragma once
#include <stdbool.h>
#include <stddef.h>

// Called after WiFi connects — registers hub with server
void api_register_hub(void);

// Tells server to open the sensor pairing window. Returns true on success.
bool api_enable_sensor_pairing(void);

// Poll server for a pending sensor whose provision_key has not been delivered yet.
// On success: fills MAC/key and optional name/zone outputs. Server clears the key
// so this is one-time only. Returns true on success.
bool api_fetch_sensor_pairing(char *out_sensor_mac, char *out_provision_key_hex,
                              char *out_name, size_t out_name_len,
                              char *out_zone, size_t out_zone_len);

// Called after ESP-NOW pairing is confirmed — tells server sensor is live
bool api_confirm_sensor(const char *sensor_mac);

// Called when sensor sends an event — forwards to server.
// Returns HTTP status code (200/201/202 = accepted), or 0 on network error.
int api_send_event(const char *sensor_mac, const char *event_type,
                   const char *severity, const char *payload_json);

// Hub-level event with no sensorMacAddress — e.g. "hub_online"
bool api_send_hub_event(const char *event_type);
