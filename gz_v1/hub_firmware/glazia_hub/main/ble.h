#pragma once

// Start BLE advertising and wait for phone to send credentials
// When credentials are received, automatically starts WiFi connection
void ble_start(void);

// Stop BLE (called after credentials received)
void ble_stop(void);
