#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * R307 Fingerprint Sensor Driver
 *
 * Communicates over UART2 (GPIO43=TX, GPIO44=RX) at 57600 baud.
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
 * Check if a fingerprint is enrolled in NVS.
 * Returns true if enrollment flag is set, false otherwise.
 */
bool fp_is_enrolled(void);

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

/**
 * Fingerprint verification task (spawned by critical_toggle_cb).
 * Runs asynchronously, verifies a fingerprint, and reverts the toggle if verification fails.
 */
void fingerprint_verify_task(void *arg);
