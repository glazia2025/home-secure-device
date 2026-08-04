#pragma once

// Call once at boot to configure GPIO and start button task
void button_init(void);

// Opens the server/ESP-NOW sensor pairing window. Caller handles auth.
void sensor_pairing_open_window(void);
