#pragma once
#include <stdint.h>

// Called when server pushes sensor MAC over WebSocket
// Adds sensor as peer and sends HELLO packet
void espnow_init(void);
void espnow_pair_sensor(const char *sensor_mac_str);

// Called by main loop when sensor event received
// Registered callback — you don't call this directly
