#!/bin/bash
sed -i '/#include "freertos\/idf_additions.h"/a #include "esp_hosted_event.h"\n#include "esp_event.h"' /home/xio/Desktop/glazia_work/glazia-prototype/gz_temp/hub_firmware/glazia_hub/main/main.c

sed -i '/static void fingerprint_init_task/i static void transport_up_handler(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data) {\n    if (id == ESP_HOSTED_EVENT_TRANSPORT_UP) {\n        xSemaphoreGive((SemaphoreHandle_t)handler_arg);\n    }\n}\n' /home/xio/Desktop/glazia_work/glazia-prototype/gz_temp/hub_firmware/glazia_hub/main/main.c

sed -i '/ble_preinit();/c \
        SemaphoreHandle_t transport_up_sem = xSemaphoreCreateBinary();\
        esp_event_handler_instance_t instance;\
        esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_UP, transport_up_handler, transport_up_sem, &instance);\
        ESP_LOGI(TAG, "Waiting for ESP-Hosted transport to be up before ble_preinit...");\
        xSemaphoreTake(transport_up_sem, portMAX_DELAY);\
        ESP_LOGI(TAG, "ESP-Hosted transport is UP! Calling ble_preinit...");\
        esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_UP, instance);\
        vSemaphoreDelete(transport_up_sem);\
        ble_preinit();' /home/xio/Desktop/glazia_work/glazia-prototype/gz_temp/hub_firmware/glazia_hub/main/main.c
