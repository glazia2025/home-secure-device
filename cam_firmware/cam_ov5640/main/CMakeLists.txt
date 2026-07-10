idf_component_register(
    SRCS
        "main.c"
        "spi_bridge.c"
        "camera_core.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_event
        esp32-camera
        driver
        esp_psram
        spi_flash
        log
)
