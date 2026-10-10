#pragma once
/**
 * display_power.h — shared owner of the two resources the MIPI-DSI display and
 * the MIPI-CSI camera must both use on the ESP32-P4:
 *
 *   1. The MIPI D-PHY LDO channel (channel 3 @ 2500 mV). One physical regulator
 *      feeds both the DSI and CSI D-PHYs, so it must be acquired exactly once.
 *   2. The board hardware I2C bus (I2C_NUM_0, GPIO7/8). On the FireBeetle 2
 *      ESP32-P4 this bus carries the camera SCCB, the GT911 touch controller and
 *      the panel backlight expander (0x45) — all on the same two pins.
 *
 * The display is always on for the life of the process and comes up first
 * (display_init() in main.c, before any WebRTC viewer can start the camera), so
 * it owns both resources permanently. The camera reuses the shared handles and
 * never acquires or releases them itself. All entry points are idempotent.
 */
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"

/* Acquire the MIPI D-PHY LDO channel once (idempotent — safe to call again). */
esp_err_t display_power_acquire_mipi_phy(void);

/* The shared LDO handle (NULL until acquired). */
esp_ldo_channel_handle_t display_power_ldo_handle(void);

/* Create (once) and return the shared I2C_NUM_0 master bus handle. Returns NULL
 * on failure. Idempotent — repeated calls return the same handle. */
i2c_master_bus_handle_t display_i2c_bus(void);
