file(GLOB UI_SRCS "ui/*.c")

idf_component_register(
    SRCS
        "main.c"
        "button.c"
        "ble.c"
        "wifi.c"
        "api_client.c"
        "camera_stream.c"
        "door_lock.c"
        "espnow.c"
        "display.c"
        "hub_sensor.c"
        "hub_control_ws.c"
        "fingerprint.c"
        "nvs_storage.c"
        ${UI_SRCS}
    INCLUDE_DIRS "." "ui"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        bt
        json
        esp32-camera
        esp_http_client
        esp_websocket_client
        driver
        esp_psram
        spi_flash
        log
        esp_lcd
        esp_lcd_touch
        esp_lcd_touch_xpt2046
        esp_timer
)
