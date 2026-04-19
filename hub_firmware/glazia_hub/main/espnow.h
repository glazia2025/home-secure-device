#pragma once
#include <stdint.h>
#include <stdbool.h>

// Initialise ESP-NOW and register callbacks.
// Sets the global PMK so encrypted peers can be added.
void espnow_init(void);

// Begin pairing a new sensor using the provision_key as its ESP-NOW LMK.
// provision_key_hex: 32-char hex string (16 bytes), e.g. "a1b2c3...".
// On ACK: promotes provisional NVS → main NVS.
// On timeout: clears provisional NVS.
void espnow_pair_sensor(const char *sensor_mac_str, const char *provision_key_hex);

// On reboot: load saved sensor MACs + LMK keys from NVS and re-add each as an
// encrypted ESP-NOW peer. Does NOT send HELLO (sensors reconnect on their own).
void espnow_reconnect_saved_sensors(void);
