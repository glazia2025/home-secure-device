idf_component_register(
    SRCS "mac_extractor.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_event esp_netif
)
