#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "spi_bridge.h"

static const char *TAG = "CAM_MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Glazia Camera Coprocessor Firmware");

    // Initialize NVS (Required by WiFi library for calibration data, even if we don't save credentials)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the event loop (Required for WiFi)
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    spi_bridge_start();

    ESP_LOGI(TAG, "Boot complete. Waiting for Hub commands over SPI...");
}
