/**
 * mac_extractor.c — Glazia Sensor MAC Extractor
 * ───────────────────────────────────────────────
 * Flash this to your sensor board ONCE to get its MAC address.
 * After flashing, open the serial monitor — the MAC is printed large.
 *
 * Copy that MAC, then run:
 *   python gen_qr.py --mac AA:BB:CC:DD:EE:FF
 *
 * That generates the QR image you'll scan on the Glazia app.
 * After getting the MAC, flash glazia_sensor.c onto the board instead.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAC_EXTRACTOR";

void app_main(void)
{
    /* Minimal init just to read WiFi MAC */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Print it big so it's impossible to miss */
    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║        SENSOR MAC ADDRESS            ║\n");
    printf("║                                      ║\n");
    printf("║   %s                  ║\n", mac_str);
    printf("║                                      ║\n");
    printf("║  Run:                                ║\n");
    printf("║  python gen_qr.py --mac %s  ║\n", mac_str);
    printf("╚══════════════════════════════════════╝\n");
    printf("\n");

    ESP_LOGI(TAG, "Sensor MAC: %s", mac_str);
    ESP_LOGI(TAG, "Now run: python gen_qr.py --mac %s", mac_str);
    ESP_LOGI(TAG, "Then flash glazia_sensor onto this board.");

    while (1) {
        /* Repeat every 5s so you don't miss it */
        ESP_LOGI(TAG, "MAC: %s", mac_str);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
