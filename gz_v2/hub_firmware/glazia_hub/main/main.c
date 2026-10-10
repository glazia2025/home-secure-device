#include "nvs_flash.h"
#include "nvs_storage.h"
#include "wifi.h"
#include "ble.h"
#include "sensor_pairing.h"
#include "display.h"
#include "door_lock.h"
#include "fingerprint.h"
#include "hub_sensor.h"
#include "webrtc_stream.h"
#include "nrf_thread.h"
#include "state.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_hosted.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"


static const char *TAG = "MAIN";

#define ENABLE_FINGERPRINT_BOOT_INIT 1
#define FINGERPRINT_INIT_DELAY_MS 3000

// Instantiate globals
hub_mode_t g_mode = MODE_IDLE;
char g_wifi_ssid[64] = {0}, g_wifi_password[64] = {0}, g_provisioning_token[64] = {0};
char g_hub_mac[18] = {0}, g_hub_secret[128] = {0}, g_home_id[64] = {0}, g_home_name[64] = {0}, g_user_name[64] = {0};
char g_pending_sensor_mac[18] = {0};
char g_pending_provision_key[33] = {0};
char g_turn_user[64] = TURN_DEFAULT_USER;
char g_turn_psw[64]  = TURN_DEFAULT_PSW;

static void fingerprint_init_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(FINGERPRINT_INIT_DELAY_MS));
    ESP_LOGI(TAG, "delayed fp_init starting");
    esp_err_t fp_err = fp_init();
    if (fp_err != ESP_OK) {
        ESP_LOGW(TAG, "Fingerprint init failed: %s", esp_err_to_name(fp_err));
    } else {
        ESP_LOGI(TAG, "Fingerprint driver ready");
        if (g_hub_secret[0] != '\0') {
            fp_start_enroll_if_needed();
        } else {
            ESP_LOGI(TAG, "Hub not yet registered — skipping boot fingerprint enrollment");
        }
    }

    vTaskDelete(NULL);
}

void app_main(void) {
    /* USB-JTAG re-enumerates when app boots; wait for host to reconnect before logging */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Boot: app_main starting");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init needs erase: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Bring up the ESP-Hosted C6 transport once, early — before the heavy PSRAM
     * bg-image memcpy and WebRTC DTLS cert-gen that follow.  WiFi and BLE both
     * take the "transport already up" fast path after this.  Factory C6 firmware
     * has BT enabled by default — no bt_controller_init/enable needed. */
    ESP_LOGI(TAG, "Bringing up ESP-Hosted C6 co-processor link...");
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        ESP_LOGE(TAG, "C6 link DOWN — WiFi/BLE unavailable this boot");
    }

    /* Copy the registration background image from Flash XIP to PSRAM now,
     * while only app_main is running.  No other task is doing Flash access
     * yet, so the shared L2 cache (Flash XIP + PSRAM) is stable and the
     * memcpy cannot race with a cache suspension from another task. */

    /* The P4 has no native Wi-Fi/BT radio (both live on the ESP-Hosted C6), so
     * ESP_MAC_WIFI_STA is not available here and fails. Use the P4's efuse base
     * MAC — always valid at boot, stable and unique. The same g_hub_mac is used
     * for the provisioning QR and server registration, so consistency is what
     * matters. */
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_BASE);
    if (mac_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac(ESP_MAC_BASE) failed: %s", esp_err_to_name(mac_err));
    }
    snprintf(g_hub_mac, sizeof(g_hub_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "HUB MAC: %s", g_hub_mac);

    /* On-P4 WebRTC streamer: pre-gen DTLS cert + reserve PSRAM stacks. The camera
     * and peer only spin up on demand when a viewer connects (viewer-ready). */
    ESP_LOGI(TAG, "webrtc_stream_init starting");
    webrtc_stream_init();

    /* Init nRF IPC UART NOW — before WiFi — so probe bytes from nRF are not lost */
    nrf_thread_preinit();

    ESP_LOGI(TAG, "wifi_platform_init starting");
    esp_err_t wifi_platform_err = wifi_platform_init();
    if (wifi_platform_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi platform init failed: %s", esp_err_to_name(wifi_platform_err));
    } else {
        ESP_LOGI(TAG, "WiFi platform ready");
    }

    /* Check credentials NOW so we can decide whether to advertise over BLE. */
    bool registered = nvs_load_credentials();
    // ble_preinit() has been moved to wifi.c (WIFI_EVENT_STA_START) to align with p4_wifi ground truth


    ESP_LOGI(TAG, "door_lock_init starting");
    esp_err_t lock_err = door_lock_init();
    if (lock_err != ESP_OK) {
        ESP_LOGW(TAG, "Door lock init failed: %s", esp_err_to_name(lock_err));
    } else {
        ESP_LOGI(TAG, "Door lock GPIO inactive");
    }

    ESP_LOGI(TAG, "display_init starting");
    display_init();
    // display_wait_ready(4000);
    ESP_LOGI(TAG, "display_init queued/done");

    ESP_LOGI(TAG, "Hub DHT22 sensor init delayed until WiFi is connected");

    if (ENABLE_FINGERPRINT_BOOT_INIT) {
        ESP_LOGI(TAG, "fingerprint delayed init task starting");
        if (xTaskCreatePinnedToCore(fingerprint_init_task, "fp_init", 4096, NULL, 5, NULL, 1) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create fingerprint init task");
        }
    } else {
        ESP_LOGW(TAG, "Fingerprint boot init disabled to keep hub boot stable");
    }

    ESP_LOGI(TAG, "sensor_pairing_init starting");
    sensor_pairing_init();
    ESP_LOGI(TAG, "sensor_pairing_init done");

    // AUTO-CONNECT LOGIC (credentials already loaded into globals above)
    if (registered) {
        ESP_LOGI(TAG, "Saved credentials found! Reconnecting...");
        g_mode = MODE_WIFI_CONNECTING;
        ESP_LOGI(TAG, "Mode transition: WIFI_CONNECTING");
        wifi_connect(g_wifi_ssid, g_wifi_password);
    } else {
        g_mode = MODE_IDLE;
        ESP_LOGI(TAG, "No saved credentials. Mode transition: IDLE, waiting for TFT BLE setup");
        display_show_setup_prompt();
    }

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
