#pragma once

#include <stdbool.h>

// No-op guard: factory C6 firmware has BT enabled at boot; the bt_controller_init
// and bt_controller_enable RPCs are not needed and always time out on the factory
// firmware. Transport must be up (connect_to_slave called in app_main). Always
// returns true.
bool ble_controller_preinit(void);

// Initialize the NimBLE host stack, GATT services, and host task at boot.
// Requires the C6 BT controller to be up (calls ble_controller_preinit() if not).
// Does NOT start advertising; call ble_start() when ready to advertise.
void ble_preinit(void);

// Start BLE advertising (hub name "GlaziaHub"). If ble_preinit() was called at
// boot this is a fast path — no transport re-init, just kicks off advertising.
// When credentials are received, automatically starts WiFi connection.
void ble_start(void);

// Stop BLE (called after credentials received before WiFi connect)
void ble_stop(void);
