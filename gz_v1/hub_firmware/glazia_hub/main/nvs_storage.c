#include "nvs_storage.h"
#include "state.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG        = "NVS";
static const char *NVS_NS     = "glazia";       // main namespace
static const char *NVS_PROV   = "glazia_prov";  // provisional namespace
static const char *NVS_THREAD = "glz_thread";   // Thread sensor namespace

// ── Main namespace ─────────────────────────────────────────────────────────

void nvs_save_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(h, "wifi_ssid",  g_wifi_ssid);
    nvs_set_str(h, "wifi_pass",  g_wifi_password);
    nvs_set_str(h, "hub_mac",    g_hub_mac);
    nvs_set_str(h, "hub_secret", g_hub_secret);
    nvs_set_str(h, "home_name",  g_home_name);
    nvs_set_str(h, "user_name",  g_user_name);
    if (g_turn_user[0]) nvs_set_str(h, "turn_user", g_turn_user);
    if (g_turn_psw[0])  nvs_set_str(h, "turn_psw",  g_turn_psw);

    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Credentials saved to NVS");
}

bool nvs_load_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }

    size_t len;

    #define LOAD(key, dest, dest_size, required) \
        len = dest_size; \
        if (nvs_get_str(h, key, dest, &len) != ESP_OK) { \
            if (required) { nvs_close(h); return false; } \
        }

    LOAD("wifi_ssid",  g_wifi_ssid,    sizeof(g_wifi_ssid),    true)
    LOAD("wifi_pass",  g_wifi_password, sizeof(g_wifi_password), true)
    LOAD("hub_secret", g_hub_secret,   sizeof(g_hub_secret),   true)
    LOAD("hub_mac",    g_hub_mac,      sizeof(g_hub_mac),      false)
    LOAD("home_name",  g_home_name,    sizeof(g_home_name),    false)
    LOAD("user_name",  g_user_name,    sizeof(g_user_name),    false)
    /* TURN creds optional — if absent, globals keep compile-time defaults from state.h */
    LOAD("turn_user",  g_turn_user,    sizeof(g_turn_user),    false)
    LOAD("turn_psw",   g_turn_psw,     sizeof(g_turn_psw),     false)

    #undef LOAD

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded credentials from NVS");
    return true;
}

void nvs_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS credentials cleared");
    }
}

void nvs_save_fingerprint(bool enrolled, const char *fingerprint_id, uint16_t slot)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for fingerprint: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(h, "fp_enrolled", enrolled ? 1 : 0);
    nvs_set_str(h, "fp_id", fingerprint_id ? fingerprint_id : "");
    nvs_set_u16(h, "fp_slot", slot);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Fingerprint metadata saved: enrolled=%d id=%s slot=%u",
             enrolled ? 1 : 0, fingerprint_id ? fingerprint_id : "", slot);
}

bool nvs_load_fingerprint(bool *out_enrolled, char *out_id, size_t out_id_len, uint16_t *out_slot)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t enrolled = 0;
    esp_err_t err = nvs_get_u8(h, "fp_enrolled", &enrolled);
    if (err != ESP_OK) {
        nvs_close(h);
        return false;
    }

    if (out_enrolled) *out_enrolled = (enrolled != 0);

    if (out_id && out_id_len > 0) {
        size_t len = out_id_len;
        if (nvs_get_str(h, "fp_id", out_id, &len) != ESP_OK) {
            out_id[0] = '\0';
        }
    }

    if (out_slot) {
        uint16_t slot = 0;
        if (nvs_get_u16(h, "fp_slot", &slot) == ESP_OK) {
            *out_slot = slot;
        } else {
            *out_slot = 0;
        }
    }

    nvs_close(h);
    return true;
}

bool nvs_load_fp_enrolled(void)
{
    uint16_t slots[5];
    return nvs_load_fingerprints(slots, 5) > 0;
}

void nvs_save_fingerprints(uint8_t count, const uint16_t *slots)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for fingerprints: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_u8(h, "fp_count", count);
    nvs_set_u8(h, "fp_enrolled", count > 0 ? 1 : 0);
    for (uint8_t i = 0; i < count; i++) {
        char key[12];
        snprintf(key, sizeof(key), "fp_slot%d", i);
        nvs_set_u16(h, key, slots[i]);
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %u fingerprint slot(s)", count);
}

uint8_t nvs_load_fingerprints(uint16_t *slots, uint8_t max_slots)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t count = 0;
    if (nvs_get_u8(h, "fp_count", &count) != ESP_OK) {
        bool enrolled = false;
        uint16_t slot = 0;
        if (nvs_load_fingerprint(&enrolled, NULL, 0, &slot) && enrolled && slot > 0 && slots && max_slots > 0) {
            slots[0] = slot;
            nvs_close(h);
            return 1;
        }
        nvs_close(h);
        return 0;
    }

    if (count > max_slots) count = max_slots;
    uint8_t loaded = 0;
    for (uint8_t i = 0; i < count; i++) {
        char key[12];
        snprintf(key, sizeof(key), "fp_slot%d", i);
        uint16_t slot = 0;
        if (nvs_get_u16(h, key, &slot) == ESP_OK && slot > 0) {
            if (slots) slots[loaded] = slot;
            loaded++;
        }
    }

    nvs_close(h);
    return loaded;
}

void nvs_save_sensors(const char macs[][18], const uint8_t keys[][16],
                      const char names[][32], const char zones[][32], int count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "sensor_count", (uint8_t)count);
    for (int i = 0; i < count; i++) {
        char mac_key[24], lmk_key[24], name_key[24], zone_key[24];
        snprintf(mac_key, sizeof(mac_key), "sensor_%d", i);
        snprintf(lmk_key, sizeof(lmk_key), "sensor_key_%d", i);
        snprintf(name_key, sizeof(name_key), "sensor_name_%d", i);
        snprintf(zone_key, sizeof(zone_key), "sensor_zone_%d", i);

        nvs_set_str(h, mac_key, macs[i]);
        nvs_set_blob(h, lmk_key, keys[i], 16);
        nvs_set_str(h, name_key, names ? names[i] : "");
        nvs_set_str(h, zone_key, zones ? zones[i] : "");
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d sensor(s) with LMK keys to NVS", count);
}

void nvs_save_sensor_enabled(int index, bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[24];
    snprintf(key, sizeof(key), "sensor_en_%d", index);
    nvs_set_u8(h, key, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool nvs_load_sensor_enabled(int index, bool default_enabled)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return default_enabled;
    char key[24];
    snprintf(key, sizeof(key), "sensor_en_%d", index);
    uint8_t value = default_enabled ? 1 : 0;
    nvs_get_u8(h, key, &value);
    nvs_close(h);
    return value != 0;
}

int nvs_load_sensors(char macs[][18], uint8_t keys[][16],
                     char names[][32], char zones[][32], int max_count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t count = 0;
    nvs_get_u8(h, "sensor_count", &count);
    if (count > max_count) count = max_count;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        char mac_key[24], lmk_key[24], name_key[24], zone_key[24];
        snprintf(mac_key, sizeof(mac_key), "sensor_%d", i);
        snprintf(lmk_key, sizeof(lmk_key), "sensor_key_%d", i);
        snprintf(name_key, sizeof(name_key), "sensor_name_%d", i);
        snprintf(zone_key, sizeof(zone_key), "sensor_zone_%d", i);

        size_t mac_len = 18;
        if (nvs_get_str(h, mac_key, macs[loaded], &mac_len) != ESP_OK) continue;

        if (keys != NULL) {
            size_t lmk_len = 16;
            if (nvs_get_blob(h, lmk_key, keys[loaded], &lmk_len) != ESP_OK) {
                // Missing LMK — skip this sensor so we don't reconnect unencrypted
                ESP_LOGW(TAG, "No LMK for sensor %d (%s) — skipping", i, macs[loaded]);
                continue;
            }
        }

        if (names != NULL) {
            size_t name_len = 32;
            if (nvs_get_str(h, name_key, names[loaded], &name_len) != ESP_OK) {
                names[loaded][0] = '\0';
            }
        }
        if (zones != NULL) {
            size_t zone_len = 32;
            if (nvs_get_str(h, zone_key, zones[loaded], &zone_len) != ESP_OK) {
                zones[loaded][0] = '\0';
            }
        }

        loaded++;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d sensor(s) from NVS", loaded);
    return loaded;
}

// ── Provisional namespace ──────────────────────────────────────────────────

void nvs_prov_save_sensor(const char *sensor_mac, const char *provision_key_hex,
                          const char *name, const char *zone)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PROV, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open provisional NVS");
        return;
    }
    nvs_set_str(h, "prov_mac", sensor_mac);
    nvs_set_str(h, "prov_key", provision_key_hex);
    nvs_set_str(h, "prov_name", name ? name : "");
    nvs_set_str(h, "prov_zone", zone ? zone : "");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Provisional sensor saved: %s", sensor_mac);
}

bool nvs_prov_load_sensor(char *out_sensor_mac, char *out_provision_key_hex,
                          char *out_name, size_t out_name_len,
                          char *out_zone, size_t out_zone_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PROV, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = 18;
    bool ok = (nvs_get_str(h, "prov_mac", out_sensor_mac, &len) == ESP_OK);
    if (ok) {
        len = 33;
        ok = (nvs_get_str(h, "prov_key", out_provision_key_hex, &len) == ESP_OK);
    }
    if (ok && out_name && out_name_len > 0) {
        len = out_name_len;
        if (nvs_get_str(h, "prov_name", out_name, &len) != ESP_OK) {
            out_name[0] = '\0';
        }
    }
    if (ok && out_zone && out_zone_len > 0) {
        len = out_zone_len;
        if (nvs_get_str(h, "prov_zone", out_zone, &len) != ESP_OK) {
            out_zone[0] = '\0';
        }
    }

    nvs_close(h);
    return ok;
}

void nvs_prov_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_PROV, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Provisional NVS cleared");
    }
}

// ── Thread sensor namespace ────────────────────────────────────────────────

void nvs_save_thread_sensors(const uint8_t eui64s[][8], const char names[][32],
                              const char zones[][32], int count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_THREAD, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "th_cnt", (uint8_t)count);
    for (int i = 0; i < count; i++) {
        char eui_key[16], name_key[16], zone_key[16];
        snprintf(eui_key,  sizeof(eui_key),  "th_eui64_%d", (uint8_t)i);
        snprintf(name_key, sizeof(name_key), "th_name_%d",  (uint8_t)i);
        snprintf(zone_key, sizeof(zone_key), "th_zone_%d",  (uint8_t)i);

        nvs_set_blob(h, eui_key,  eui64s[i], 8);
        nvs_set_str(h,  name_key, names ? names[i] : "");
        nvs_set_str(h,  zone_key, zones ? zones[i] : "");
    }

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved %d Thread sensor(s) to NVS", count);
}

int nvs_load_thread_sensors(uint8_t eui64s[][8], char names[][32],
                             char zones[][32], int max_count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_THREAD, NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t count = 0;
    nvs_get_u8(h, "th_cnt", &count);
    if (count > max_count) count = max_count;

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        char eui_key[16], name_key[16], zone_key[16];
        snprintf(eui_key,  sizeof(eui_key),  "th_eui64_%d", (uint8_t)i);
        snprintf(name_key, sizeof(name_key), "th_name_%d",  (uint8_t)i);
        snprintf(zone_key, sizeof(zone_key), "th_zone_%d",  (uint8_t)i);

        size_t eui_len = 8;
        if (nvs_get_blob(h, eui_key, eui64s[loaded], &eui_len) != ESP_OK) continue;

        if (names) {
            size_t len = 32;
            if (nvs_get_str(h, name_key, names[loaded], &len) != ESP_OK)
                names[loaded][0] = '\0';
        }
        if (zones) {
            size_t len = 32;
            if (nvs_get_str(h, zone_key, zones[loaded], &len) != ESP_OK)
                zones[loaded][0] = '\0';
        }
        loaded++;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d Thread sensor(s) from NVS", loaded);
    return loaded;
}
