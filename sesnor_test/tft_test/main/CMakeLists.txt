idf_component_register(
    SRCS "tft_test.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer
)
