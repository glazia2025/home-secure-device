idf_component_register(
    SRCS
        "tft_test.c"
        "ui/screens.c"
        "ui/styles.c"
        "ui/images.c"
        "ui/ui.c"
        "ui/eez-flow.cpp"
    INCLUDE_DIRS "." "ui"
    REQUIRES esp_lcd driver esp_timer
)
