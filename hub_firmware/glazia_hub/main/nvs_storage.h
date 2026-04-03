#pragma once
#include <stdbool.h>

// Save all hub credentials to NVS after successful registration
void nvs_save_credentials(void);

// Load credentials from NVS into global state
// Returns true if valid credentials were found, false if NVS is empty
bool nvs_load_credentials(void);

// Wipe all saved credentials (useful for factory reset)
void nvs_clear_credentials(void);
