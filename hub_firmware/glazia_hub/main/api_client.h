#pragma once
#include <stdbool.h>

// Called after WiFi connects — registers hub with server
void api_register_hub(void);

// Called after 2nd button press — tells server to open pairing window
void api_enable_sensor_pairing(void);

// Poll server for a pending sensor whose provision_key has not been delivered yet.
// On success: fills out_sensor_mac (18 bytes) and out_provision_key_hex (33 bytes),
// server clears the key so this is one-time only. Returns true on success.
bool api_fetch_sensor_pairing(char *out_sensor_mac, char *out_provision_key_hex);

// Called when sensor sends an event — forwards to server
void api_send_event(const char *sensor_mac, const char *event_type,
                    const char *severity);
