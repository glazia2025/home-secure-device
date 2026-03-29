#pragma once

// Called after WiFi connects — registers hub with server
void api_register_hub(void);

// Called after 2nd button press — enables sensor pairing window on server
void api_enable_sensor_pairing(void);

// Called after ESP-NOW handshake — tells server sensor is connected
void api_acknowledge_sensor(const char *sensor_mac);

// Called when sensor sends an event — forwards to server
void api_send_event(const char *sensor_mac, const char *event_type,
                    const char *severity);
