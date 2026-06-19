#pragma once
#include <stdbool.h>
#include <stdint.h>

// Call once at boot
void display_init(void);

// Wait until display init has either completed or failed.
bool display_wait_ready(uint32_t timeout_ms);

// Show two lines on the TFT — maps to sensor data panel
// line1 = data text, line2 = location/context (or NULL)
void display_show(const char *line1, const char *line2);

// Show first-boot setup state without changing hub mode.
void display_show_setup_prompt(void);

// Show auth/fingerprint state in the lower panel.
void display_fingerprint_status(const char *message);

// Push hub location to the display — maps to hub_location_label
// Sends: HUB_LOC:<home_name>
void display_hub_location(const char *home_name);

// Push the account name to the dashboard greeting. Only the first name is shown.
void display_user_name(const char *user_name);

// Push current sensor table to the display — maps to sensor_list_label
// Reads sensor state from espnow module and sends: SENSORS:S1|Unknown|ON;...
void display_sensor_list(void);

// Push the source sensor of the latest event — maps to hub_location_label
// Sends: SENSOR_LOC:<mac_str>
void display_sensor_location(const char *mac_str);

// Generated UI navigation/update helpers.
void display_show_dashboard(bool online);
void display_show_fingerprint_screen(const char *title, const char *prompt);
void display_fingerprint_phase(const char *phase, const char *message);
void display_fingerprint_progress(uint8_t percent);
void display_update_temp_hum(float temp, float hum);
void display_update_aqi(float aqi, const char *state);
void display_refresh_sensor_nodes(void);
void display_sensor_added_notification(const char *name);
void display_clear_sensor_notifications(void);
