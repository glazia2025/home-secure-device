#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * R307 Fingerprint Sensor Driver
 *
 * Communicates over UART2 at 57600 baud.
 * Wiring: R307 TX -> GPIO2 (ESP RX), R307 RX -> GPIO1 (ESP TX).
 * Stores one master template in R307's onboard flash (slot 1).
 */

/**
 * Initialize UART2 and R307 sensor.
 * Must be called once before any other fingerprint functions.
 */
esp_err_t fp_init(void);

/**
 * Enroll a new fingerprint (double-scan required).
 * Displays prompts on the TFT via callback.
 * Returns ESP_OK on success, ESP_FAIL if enrollment fails.
 */
esp_err_t fp_enroll(void);

/**
 * Verify a fingerprint against the enrolled template.
 * Scans once, searches the R307 template database.
 * Returns ESP_OK if match found, ESP_FAIL if no match.
 */
esp_err_t fp_verify(void);

/**
 * Verify only the admin fingerprint.
 * The first saved fingerprint slot is the admin slot.
 */
esp_err_t fp_verify_admin(void);

/**
 * Check if a fingerprint is enrolled in NVS.
 * Returns true if enrollment flag is set, false otherwise.
 */
bool fp_is_enrolled(void);

/**
 * Start post-registration enrollment if no master fingerprint exists.
 * Returns true when an enrollment task was started or already running.
 */
bool fp_start_enroll_if_needed(void);

/**
 * Display callback for showing TFT prompts during enrollment/verification.
 * This is assigned internally; the caller does not need to manage it.
 * Defined to match the signature in display.h: void (*callback)(const char *msg)
 */
typedef void (*fp_display_cb)(const char *msg);

/**
 * Set the display callback (called by display.c during init).
 */
void fp_set_display_cb(fp_display_cb cb);
