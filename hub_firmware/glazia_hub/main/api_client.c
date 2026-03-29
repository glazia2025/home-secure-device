#include "api_client.h"
#include "state.h"
#include "display.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "API";

// Response buffer
#define RESP_BUF_SIZE 1024
static char resp_buf[RESP_BUF_SIZE];
static int  resp_len = 0;

// ── HTTP event handler ────────────────────────────────────────────────────

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp_len + evt->data_len < RESP_BUF_SIZE) {
            memcpy(resp_buf + resp_len, evt->data, evt->data_len);
            resp_len += evt->data_len;
            resp_buf[resp_len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ── Generic POST helper ───────────────────────────────────────────────────

static int do_post(const char *url, const char *body,
                   const char *extra_header_key, const char *extra_header_val)
{
    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    esp_http_client_config_t config = {
        .url            = url,
        .event_handler  = http_event_handler,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);

    if (extra_header_key && extra_header_val) {
        esp_http_client_set_header(client, extra_header_key, extra_header_val);
    }

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "POST %s → %d", url, status);
    ESP_LOGI(TAG, "Response: %s", resp_buf);
    return status;
}

// ── Register hub ──────────────────────────────────────────────────────────

void api_register_hub(void)
{
    ESP_LOGI(TAG, "Registering hub with server...");
    display_show("Registering", "Please wait...");

    char url[128];
    snprintf(url, sizeof(url), "%s/api/device/hubs/register", SERVER_BASE);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"hubMacAddress\":\"%s\",\"provisioningToken\":\"%s\"}",
        g_hub_mac, g_provisioning_token);

    int status = do_post(url, body, NULL, NULL);

    if (status == 200) {
        // Parse response — extract hubSecret, homeId, homeName
        cJSON *root = cJSON_Parse(resp_buf);
        if (root) {
            cJSON *secret   = cJSON_GetObjectItem(root, "hubSecret");
            cJSON *home_id  = cJSON_GetObjectItem(root, "homeId");
            cJSON *home_name = cJSON_GetObjectItem(root, "homeName");

            if (secret)    strncpy(g_hub_secret, secret->valuestring, sizeof(g_hub_secret) - 1);
            if (home_id)   strncpy(g_home_id, home_id->valuestring, sizeof(g_home_id) - 1);
            if (home_name) strncpy(g_home_name, home_name->valuestring, sizeof(g_home_name) - 1);

            cJSON_Delete(root);
        }

        g_mode = MODE_OPERATIONAL;
        display_show("Hub Ready!", g_home_name);
        ESP_LOGI(TAG, "Hub registered — Home: %s", g_home_name);

    } else {
        display_show("Reg Failed", "Check server");
        ESP_LOGE(TAG, "Registration failed with status %d", status);
    }
}

// ── Enable sensor pairing ─────────────────────────────────────────────────

void api_enable_sensor_pairing(void)
{
    ESP_LOGI(TAG, "Enabling sensor pairing mode on server...");

    char url[128];
    snprintf(url, sizeof(url), "%s/api/device/hubs/sensor-pairing-mode", SERVER_BASE);

    // Build extra headers for hub auth
    // We need both X-Hub-Mac-Address and X-Hub-Secret
    // We'll set them manually since do_post only supports one extra header
    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    esp_http_client_config_t config = {
        .url           = url,
        .event_handler = http_event_handler,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);
    esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    esp_http_client_set_header(client, "X-Hub-Secret", g_hub_secret);
    esp_http_client_set_post_field(client, "{}", 2);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Sensor pairing mode enabled on server");
    } else {
        ESP_LOGE(TAG, "Failed to enable sensor pairing: %d", status);
    }
}

// ── Acknowledge sensor ────────────────────────────────────────────────────

void api_acknowledge_sensor(const char *sensor_mac)
{
    ESP_LOGI(TAG, "Acknowledging sensor %s", sensor_mac);

    char url[128];
    snprintf(url, sizeof(url), "%s/api/device/hubs/sensors/acknowledge", SERVER_BASE);

    char body[128];
    snprintf(body, sizeof(body), "{\"sensorMacAddress\":\"%s\"}", sensor_mac);

    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    esp_http_client_config_t config = {
        .url           = url,
        .event_handler = http_event_handler,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);
    esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    esp_http_client_set_header(client, "X-Hub-Secret", g_hub_secret);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Sensor acknowledged on server");
    } else {
        ESP_LOGE(TAG, "Acknowledge failed: %d", status);
    }
}

// ── Send sensor event ─────────────────────────────────────────────────────

void api_send_event(const char *sensor_mac, const char *event_type,
                    const char *severity)
{
    ESP_LOGI(TAG, "Sending event: %s from %s", event_type, sensor_mac);

    char url[128];
    snprintf(url, sizeof(url), "%s/api/device/hubs/events", SERVER_BASE);

    char body[256];
    snprintf(body, sizeof(body),
        "{\"sensorMacAddress\":\"%s\",\"eventType\":\"%s\",\"severity\":\"%s\",\"payload\":{}}",
        sensor_mac, event_type, severity);

    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    esp_http_client_config_t config = {
        .url           = url,
        .event_handler = http_event_handler,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);
    esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    esp_http_client_set_header(client, "X-Hub-Secret", g_hub_secret);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        ESP_LOGI(TAG, "Event sent successfully");
    } else {
        ESP_LOGE(TAG, "Event send failed: %d", status);
    }
}
