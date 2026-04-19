#include "api_client.h"
#include "state.h"
#include "display.h"
#include "nvs_storage.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "API";

static char resp_buf[1024];
static int  resp_len  = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp_len + evt->data_len < (int)sizeof(resp_buf) - 1) {
            memcpy(resp_buf + resp_len, evt->data, evt->data_len);
            resp_len += evt->data_len;
            resp_buf[resp_len] = '\0';
        }
    }
    return ESP_OK;
}

// ── Internal helpers ───────────────────────────────────────────────────────

static int do_request(esp_http_client_method_t method, const char *path,
                      const char *body,
                      const char *extra_hdr_key, const char *extra_hdr_val)
{
    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    char url[192];
    snprintf(url, sizeof(url), "%s%s", SERVER_BASE, path);

    esp_http_client_config_t config = {
        .url           = url,
        .event_handler = http_event_handler,
        .method        = method,
        .timeout_ms    = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type",     "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);
    esp_http_client_set_header(client, "X-Hub-Secret",     g_hub_secret);

    if (strlen(g_hub_mac) > 0) {
        esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    }
    if (extra_hdr_key && extra_hdr_val) {
        esp_http_client_set_header(client, extra_hdr_key, extra_hdr_val);
    }
    if (body) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s %s failed: %s",
                 method == HTTP_METHOD_GET ? "GET" : "POST",
                 path, esp_err_to_name(err));
        status = -1;
    }

    esp_http_client_cleanup(client);
    return status;
}

static inline int do_post(const char *path, const char *body,
                           const char *hk, const char *hv)
{
    return do_request(HTTP_METHOD_POST, path, body, hk, hv);
}

static inline int do_get(const char *path)
{
    return do_request(HTTP_METHOD_GET, path, NULL, NULL, NULL);
}

// ── Public API ─────────────────────────────────────────────────────────────

void api_register_hub(void)
{
    ESP_LOGI(TAG, "Registering hub...");
    display_show("Registering", "Please wait...");

    char body[256];
    snprintf(body, sizeof(body),
             "{\"hubMacAddress\":\"%s\",\"provisioningToken\":\"%s\"}",
             g_hub_mac, g_provisioning_token);

    int status = do_post("/api/device/hubs/register", body, NULL, NULL);

    if (status == 200 || status == 201) {
        cJSON *root = cJSON_Parse(resp_buf);
        if (root) {
            cJSON *secret = cJSON_GetObjectItem(root, "hubSecret");
            cJSON *home   = cJSON_GetObjectItem(root, "home");

            if (cJSON_IsString(secret) && secret->valuestring)
                strncpy(g_hub_secret, secret->valuestring, sizeof(g_hub_secret) - 1);

            if (cJSON_IsObject(home)) {
                cJSON *id   = cJSON_GetObjectItem(home, "id");
                cJSON *name = cJSON_GetObjectItem(home, "name");
                if (cJSON_IsString(id)   && id->valuestring)
                    strncpy(g_home_id,   id->valuestring,   sizeof(g_home_id)   - 1);
                if (cJSON_IsString(name) && name->valuestring)
                    strncpy(g_home_name, name->valuestring, sizeof(g_home_name) - 1);
            }

            cJSON_Delete(root);
        }

        nvs_save_credentials();
        g_mode = MODE_OPERATIONAL;
        display_show("Hub Ready!", g_home_name);
        ESP_LOGI(TAG, "Hub registered! Home: %s  Secret: %.8s...", g_home_name, g_hub_secret);
    } else {
        display_show("Reg Failed", "Check Server");
        ESP_LOGE(TAG, "Registration failed: HTTP %d", status);
    }
}

void api_enable_sensor_pairing(void)
{
    ESP_LOGI(TAG, "Opening sensor pairing window on server...");
    int status = do_post("/api/device/hubs/sensor-pairing-mode", "{}", NULL, NULL);
    if (status == 200 || status == 201) {
        ESP_LOGI(TAG, "Server pairing mode active");
        display_show("PAIRING MODE", "Waiting for app");
    } else {
        ESP_LOGE(TAG, "Failed to enable pairing: HTTP %d", status);
    }
}

bool api_fetch_sensor_pairing(char *out_sensor_mac, char *out_provision_key_hex)
{
    int status = do_get("/api/device/hubs/pending-sensor");
    if (status != 200) {
        ESP_LOGD(TAG, "No pending sensor (HTTP %d)", status);
        return false;
    }

    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) return false;

    cJSON *mac = cJSON_GetObjectItem(root, "sensorMacAddress");
    cJSON *key = cJSON_GetObjectItem(root, "provisionKey");

    bool ok = cJSON_IsString(mac) && mac->valuestring &&
              cJSON_IsString(key) && key->valuestring;

    if (ok) {
        strncpy(out_sensor_mac,         mac->valuestring, 17);
        strncpy(out_provision_key_hex,  key->valuestring, 32);
        out_sensor_mac[17]        = '\0';
        out_provision_key_hex[32] = '\0';
        ESP_LOGI(TAG, "Fetched pending sensor: %s", out_sensor_mac);
    }

    cJSON_Delete(root);
    return ok;
}

void api_send_event(const char *sensor_mac, const char *event_type, const char *severity)
{
    ESP_LOGI(TAG, "Sending event: %s from %s", event_type, sensor_mac);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"sensorMacAddress\":\"%s\",\"eventType\":\"%s\",\"severity\":\"%s\",\"payload\":{}}",
             sensor_mac, event_type, severity);
    do_post("/api/device/hubs/events", body, NULL, NULL);
}
