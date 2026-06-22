#include "api_client.h"
#include "state.h"
#include "display.h"
#include "hub_control_ws.h"
#include "nvs_storage.h"
#include "fingerprint.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "API";

// ── Control lane (register, pairing, pending-sensor poll) ────────────────
static char              resp_buf[1024];
static int               resp_len      = 0;
static SemaphoreHandle_t s_api_mutex   = NULL;

// ── Event lane (send_event, confirm_sensor) ───────────────────────────────
// Separate buffer + mutex so event floods never block the control lane.
static char              s_evt_resp_buf[256];
static int               s_evt_resp_len = 0;
static SemaphoreHandle_t s_evt_mutex   = NULL;

static const char *response_user_name(cJSON *root)
{
    cJSON *user = cJSON_GetObjectItem(root, "user");
    cJSON *owner = cJSON_GetObjectItem(root, "owner");
    cJSON *name = NULL;

    if (cJSON_IsObject(user)) {
        name = cJSON_GetObjectItem(user, "name");
    }
    if (!cJSON_IsString(name) && cJSON_IsObject(owner)) {
        name = cJSON_GetObjectItem(owner, "name");
    }
    if (!cJSON_IsString(name)) {
        name = cJSON_GetObjectItem(root, "userName");
    }

    return cJSON_IsString(name) && name->valuestring ? name->valuestring : NULL;
}

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

static esp_err_t evt_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_evt_resp_len + evt->data_len < (int)sizeof(s_evt_resp_buf) - 1) {
            memcpy(s_evt_resp_buf + s_evt_resp_len, evt->data, evt->data_len);
            s_evt_resp_len += evt->data_len;
            s_evt_resp_buf[s_evt_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

// ── Internal helpers ───────────────────────────────────────────────────────

static int do_request(esp_http_client_method_t method, const char *path,
                      const char *body,
                      const char *extra_hdr_key, const char *extra_hdr_val)
{
    if (s_api_mutex == NULL) {
        s_api_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_api_mutex, portMAX_DELAY);

    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    char url[192];
    snprintf(url, sizeof(url), "%s%s", SERVER_BASE, path);
    const char *method_name = method == HTTP_METHOD_GET ? "GET" : "POST";
    bool is_pending_sensor_poll = strcmp(path, "/api/device/hubs/pending-sensor") == 0;
    bool log_request = !is_pending_sensor_poll;

    if (log_request) {
        ESP_LOGI(TAG, "HTTP %s %s starting", method_name, path);
    }

    esp_http_client_config_t config = {
        .url               = url,
        .event_handler     = http_event_handler,
        .method            = method,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
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
        // ESP-IDF HTTP client returns ESP_ERR_NOT_SUPPORTED for 401 responses when no
        // HTTP auth credentials are configured. The status code is still valid — propagate it.
        if (err == ESP_ERR_NOT_SUPPORTED && status >= 400 && status < 600) {
            if (log_request) {
                ESP_LOGW(TAG, "HTTP %s %s server error: status=%d", method_name, path, status);
            }
        } else {
            ESP_LOGE(TAG, "HTTP %s %s transport failed: %s",
                     method_name, path, esp_err_to_name(err));
            status = -1;
        }
    } else {
        if (log_request || status != 404) {
            ESP_LOGI(TAG, "HTTP %s %s completed: status=%d bytes=%d",
                     method_name, path, status, resp_len);
        }
    }

    esp_http_client_cleanup(client);
    xSemaphoreGive(s_api_mutex);
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

static int do_event_request(const char *path, const char *body)
{
    if (s_evt_mutex == NULL) {
        s_evt_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_evt_mutex, portMAX_DELAY);

    s_evt_resp_len = 0;
    memset(s_evt_resp_buf, 0, sizeof(s_evt_resp_buf));

    char url[192];
    snprintf(url, sizeof(url), "%s%s", SERVER_BASE, path);
    ESP_LOGI(TAG, "HTTP POST %s starting (event lane)", path);

    esp_http_client_config_t config = {
        .url               = url,
        .event_handler     = evt_http_event_handler,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type",     "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);
    esp_http_client_set_header(client, "X-Hub-Secret",     g_hub_secret);
    if (strlen(g_hub_mac) > 0) {
        esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    }
    if (body) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_SUPPORTED && status >= 400 && status < 600) {
            ESP_LOGW(TAG, "HTTP POST %s server error: status=%d", path, status);
        } else {
            ESP_LOGE(TAG, "HTTP POST %s transport failed: %s", path, esp_err_to_name(err));
            status = -1;
        }
    } else {
        ESP_LOGI(TAG, "HTTP POST %s completed: status=%d bytes=%d",
                 path, status, s_evt_resp_len);
    }

    esp_http_client_cleanup(client);
    xSemaphoreGive(s_evt_mutex);
    return status;
}

// ── Public API ─────────────────────────────────────────────────────────────

void api_register_hub(void)

{
    size_t token_len = strlen(g_provisioning_token);
    ESP_LOGI(TAG, "Registering hub mac=%s token_len=%u", g_hub_mac, (unsigned)token_len);

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

            if (cJSON_IsString(secret) && secret->valuestring) {
                strncpy(g_hub_secret, secret->valuestring, sizeof(g_hub_secret) - 1);
                g_hub_secret[sizeof(g_hub_secret) - 1] = '\0';
            }

            cJSON *id = NULL;
            cJSON *name = NULL;
            if (cJSON_IsObject(home)) {
                id = cJSON_GetObjectItem(home, "id");
                name = cJSON_GetObjectItem(home, "name");
            }
            if (!cJSON_IsString(id)) id = cJSON_GetObjectItem(root, "homeId");
            if (!cJSON_IsString(name)) name = cJSON_GetObjectItem(root, "homeName");

            if (cJSON_IsString(id) && id->valuestring) {
                strncpy(g_home_id, id->valuestring, sizeof(g_home_id) - 1);
                g_home_id[sizeof(g_home_id) - 1] = '\0';
            }
            if (cJSON_IsString(name) && name->valuestring) {
                strncpy(g_home_name, name->valuestring, sizeof(g_home_name) - 1);
                g_home_name[sizeof(g_home_name) - 1] = '\0';
            } else {
                ESP_LOGW(TAG, "Registration response missing home name; expected home.name or homeName");
            }

            const char *user_name = response_user_name(root);
            if (user_name && user_name[0] != '\0') {
                strncpy(g_user_name, user_name, sizeof(g_user_name) - 1);
                g_user_name[sizeof(g_user_name) - 1] = '\0';
            }

            cJSON_Delete(root);
        }

        if (strlen(g_hub_secret) == 0) {
            ESP_LOGE(TAG, "Registration response missing hubSecret");
            return;
        }

        nvs_save_credentials();
        g_mode = MODE_OPERATIONAL;
        ESP_LOGI(TAG, "Mode transition: OPERATIONAL after registration");
        display_user_name(g_user_name);
        display_show_dashboard(true);
        display_hub_location(g_home_name);
        fp_start_enroll_if_needed();
        hub_control_ws_start();
        ESP_LOGI(TAG, "[DEV-LOG:REMOVE_BEFORE_PROD] Hub registered! Home: %s  Secret prefix: %.8s...", g_home_name, g_hub_secret);
    } else {
        ESP_LOGE(TAG, "Registration failed: HTTP %d", status);
    }
}

bool api_enable_sensor_pairing(void)
{
    ESP_LOGI(TAG, "Opening sensor pairing window on server...");
    int status = do_post("/api/device/hubs/sensor-pairing-mode", "{}", NULL, NULL);
    if (status == 200 || status == 201) {
        ESP_LOGI(TAG, "Server pairing mode active");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to enable pairing: HTTP %d", status);
        return false;
    }
}

bool api_fetch_sensor_pairing(char *out_sensor_mac, char *out_provision_key_hex,
                              char *out_name, size_t out_name_len,
                              char *out_zone, size_t out_zone_len)
{
    static uint32_t no_pending_count = 0;
    int status = do_get("/api/device/hubs/pending-sensor");
    if (status != 200) {
        no_pending_count++;
        if (status == 404) {
            if (no_pending_count == 1 || (no_pending_count % 10) == 0) {
                ESP_LOGI(TAG, "No pending sensor yet (poll count=%u)", (unsigned)no_pending_count);
            }
        } else {
            ESP_LOGW(TAG, "Pending sensor poll failed: HTTP %d", status);
        }
        return false;
    }
    no_pending_count = 0;

    cJSON *root = cJSON_Parse(resp_buf);
    if (!root) {
        ESP_LOGW(TAG, "Pending sensor response JSON parse failed");
        return false;
    }

    cJSON *mac  = cJSON_GetObjectItem(root, "sensorMacAddress");
    cJSON *key  = cJSON_GetObjectItem(root, "provisionKey");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *zone = cJSON_GetObjectItem(root, "zone");

    bool ok = cJSON_IsString(mac) && mac->valuestring &&
              cJSON_IsString(key) && key->valuestring;

    if (ok) {
        strncpy(out_sensor_mac,         mac->valuestring, 17);
        strncpy(out_provision_key_hex,  key->valuestring, 32);
        out_sensor_mac[17]        = '\0';
        out_provision_key_hex[32] = '\0';
        if (out_name && out_name_len > 0) {
            if (cJSON_IsString(name) && name->valuestring) {
                strncpy(out_name, name->valuestring, out_name_len - 1);
                out_name[out_name_len - 1] = '\0';
            } else {
                out_name[0] = '\0';
            }
        }
        if (out_zone && out_zone_len > 0) {
            if (cJSON_IsString(zone) && zone->valuestring) {
                strncpy(out_zone, zone->valuestring, out_zone_len - 1);
                out_zone[out_zone_len - 1] = '\0';
            } else {
                out_zone[0] = '\0';
            }
        }
        ESP_LOGI(TAG, "Fetched pending sensor: %s provision_key_len=%u zone=%s",
                 out_sensor_mac, (unsigned)strlen(out_provision_key_hex),
                 (out_zone && out_zone[0]) ? out_zone : "Unknown");
    } else {
        ESP_LOGW(TAG, "Pending sensor response missing sensorMacAddress/provisionKey");
    }

    cJSON_Delete(root);
    return ok;
}

bool api_confirm_sensor(const char *sensor_mac)
{
    ESP_LOGI(TAG, "Confirming sensor %s with server", sensor_mac);
    char body[64];
    snprintf(body, sizeof(body), "{\"sensorMacAddress\":\"%s\"}", sensor_mac);
    int status = do_event_request("/api/device/hubs/sensors/confirm", body);
    bool ok = status == 200 || status == 201;
    if (ok) {
        ESP_LOGI(TAG, "Sensor %s confirmed on server", sensor_mac);
    } else {
        ESP_LOGW(TAG, "Sensor confirm failed: status=%d sensor=%s", status, sensor_mac);
    }
    return ok;
}

bool api_send_event(const char *sensor_mac, const char *event_type, const char *severity, const char *payload_json)
{
    ESP_LOGI(TAG, "Sending event: %s from %s", event_type, sensor_mac);
    char body[384];
    const char *payload = (payload_json && payload_json[0] == '{') ? payload_json : "{}";
    snprintf(body, sizeof(body),
             "{\"sensorMacAddress\":\"%s\",\"eventType\":\"%s\",\"severity\":\"%s\",\"payload\":%s}",
             sensor_mac, event_type, severity, payload);
    int status = do_event_request("/api/device/hubs/events", body);
    bool ok = status == 200 || status == 201 || status == 202;
    if (ok) {
        ESP_LOGI(TAG, "Event accepted by server: status=%d sensor=%s", status, sensor_mac);
    } else {
        ESP_LOGW(TAG, "Event send failed: status=%d sensor=%s", status, sensor_mac);
    }
    return ok;
}
