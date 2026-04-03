#include "api_client.h"
#include "state.h"
#include "display.h"
#include "nvs_storage.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "API";

#define SERVER_BASE "http://10.182.205.6:8000"
#define DEVICE_API_KEY "glazia-dev-key"

static char resp_buf[1024];
static int resp_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp_len + evt->data_len < sizeof(resp_buf) - 1) {
            memcpy(resp_buf + resp_len, evt->data, evt->data_len);
            resp_len += evt->data_len;
            resp_buf[resp_len] = '\0';
        }
    }
    return ESP_OK;
}

static int do_post(const char *path, const char *body,
                   const char *h2_key, const char *h2_val)
{
    resp_len = 0;
    memset(resp_buf, 0, sizeof(resp_buf));

    char url[128];
    snprintf(url, sizeof(url), "%s%s", SERVER_BASE, path);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Api-Key", DEVICE_API_KEY);

    if (h2_key && h2_val) {
        esp_http_client_set_header(client, h2_key, h2_val);
    }
    if (strlen(g_hub_mac) > 0) {
        esp_http_client_set_header(client, "X-Hub-Mac-Address", g_hub_mac);
    }

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST to %s failed: %s", path, esp_err_to_name(err));
        status = -1;
    }

    esp_http_client_cleanup(client);
    return status;
}

void api_register_hub(void) {
    ESP_LOGI(TAG, "Registering hub...");
    display_show("Registering", "Please wait...");

    char body[256];
    snprintf(body, sizeof(body), "{\"hubMacAddress\":\"%s\",\"provisioningToken\":\"%s\"}",
             g_hub_mac, g_provisioning_token);

    int status = do_post("/api/device/hubs/register", body, NULL, NULL);

    if (status == 200 || status == 201) {
        cJSON *root = cJSON_Parse(resp_buf);
        if (root) {
            cJSON *s = cJSON_GetObjectItem(root, "hubSecret");
            cJSON *id = cJSON_GetObjectItem(root, "homeId");
            cJSON *n = cJSON_GetObjectItem(root, "homeName");

            if (cJSON_IsString(s) && s->valuestring) strncpy(g_hub_secret, s->valuestring, sizeof(g_hub_secret)-1);
            if (cJSON_IsString(id) && id->valuestring) strncpy(g_home_id, id->valuestring, sizeof(g_home_id)-1);
            if (cJSON_IsString(n) && n->valuestring) strncpy(g_home_name, n->valuestring, sizeof(g_home_name)-1);

            cJSON_Delete(root);
        }

        // Store everything in permanent memory!
        nvs_save_credentials();

        g_mode = MODE_OPERATIONAL;
        display_show("Hub Ready!", g_home_name);
        ESP_LOGI(TAG, "Hub Registered! Home: %s", g_home_name);
    } else {
        display_show("Reg Failed", "Check Server");
        ESP_LOGE(TAG, "Registration failed: %d", status);
    }
}

void api_enable_sensor_pairing(void) {
    ESP_LOGI(TAG, "Opening sensor pairing window...");
    int status = do_post("/api/device/hubs/sensor-pairing-mode", "{}", "X-Hub-Secret", g_hub_secret);
    if (status == 200) {
        ESP_LOGI(TAG, "Pairing mode active on server");
        display_show("PAIRING MODE", "Scan Sensor QR");
    } else {
        ESP_LOGE(TAG, "Failed to enable pairing: %d", status);
    }
}

void api_acknowledge_sensor(const char *sensor_mac) {
    ESP_LOGI(TAG, "Acknowledging sensor: %s", sensor_mac);
    char body[128];
    snprintf(body, sizeof(body), "{\"sensorMacAddress\":\"%s\"}", sensor_mac);
    do_post("/api/device/hubs/sensors/acknowledge", body, "X-Hub-Secret", g_hub_secret);
}

void api_send_event(const char *sensor_mac, const char *event_type, const char *severity) {
    ESP_LOGI(TAG, "Sending event: %s", event_type);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"sensorMacAddress\":\"%s\",\"eventType\":\"%s\",\"severity\":\"%s\",\"payload\":{}}",
             sensor_mac, event_type, severity);
    do_post("/api/device/hubs/events", body, "X-Hub-Secret", g_hub_secret);
}
