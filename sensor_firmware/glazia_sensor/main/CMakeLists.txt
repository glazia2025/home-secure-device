idf_component_register(
    SRCS "glazia_sensor.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        nvs_flash
        esp_event
        esp_netif
        driver
        bt
        log
)
