# Native LVGL UI sources (ported from the ex-S3 display coprocessor).
file(GLOB UI_SRCS "ui/*.c")

idf_component_register(
    SRCS
        "main.c"
        "sensor_pairing.c"
        "ble.c"
        "wifi.c"
        "api_client.c"
        "door_lock.c"
        "hub_sensor.c"
        "aqi_sensor.c"
        "metrics_history.c"
        "hub_control_ws.c"
        "camera_csi.c"
        "webrtc_stream.c"
        "esp_peer_log_shim.c"
        "fingerprint.c"
        "nvs_storage.c"
        "nrf_ipc.c"
        "nrf_thread.c"
        # Native MIPI-DSI (ILI9881C + GT911) display: LVGL renderer + shared
        # LDO/I2C owner. Replaces the old display_uart.c UART bridge to the S3.
        "display.c"
        "display_power.c"
        ${UI_SRCS}
        "freertos_p4_tcm_fix.c"
    INCLUDE_DIRS "." "ui"
    REQUIRES
        nvs_flash
        esp_wifi_remote
        esp_event
        esp_netif
        bt
        json
        esp_http_client
        esp_websocket_client
        driver
        esp_psram
        spi_flash
        log
        esp_timer
        esp_adc
        # Native OV5647 MIPI-CSI capture + hardware H.264 + WebRTC (replaces the
        # old external camera co-processor). esp_ldo_regulator lives in
        # esp_hw_support on IDF 5.3, so it needs no explicit require. esp_mm
        # provides esp_cache_msync.
        esp_driver_cam
        esp_driver_isp
        esp_driver_i2c
        esp_mm
        esp_sccb_intf
        esp_cam_sensor
        esp_h264
        esp_peer
        # Native MIPI-DSI display + capacitive touch (LVGL rendered on the P4).
        esp_lcd
        esp_lcd_touch
        esp_lcd_touch_gt911
        esp_lcd_ili9881c
        esp_lvgl_port
        lvgl
)

# IDF 5.3 P4 TCM bug: redirect stack/TCB validation to also accept TCM addresses.
# See freertos_p4_tcm_fix.c for full explanation.
target_link_options(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=xPortcheckValidStackMem
    -Wl,--wrap=xPortCheckValidTCBMem
    # esp_peer's prebuilt libpeer_default.a references an external esp_log(), which
    # we provide in esp_peer_log_shim.c (compiled into libmain.a). The prebuilt is
    # scanned after libmain.a in the single-pass link, so force esp_log to be a
    # required symbol up front — this pulls the shim object early so the prebuilt
    # resolves against it. See esp_peer_log_shim.c.
    -u esp_log
)

# NOTE vs the ESP32-S3 hub (gz_v1):
#  - espnow.c is dropped: ESP-NOW is disabled (CONFIG_ESPNOW_ENABLE=0, Thread is
#    used) and every espnow_* call site is compiled out, so nothing links to it.
#    This also avoids depending on ESP-NOW over esp-hosted on the P4.
#  - display.c + ui/ (LVGL) run natively here driving a MIPI-DSI ILI9881C panel
#    + GT911 touch (vs gz_v1's SPI ILI9341/XPT2046). The ex-S3 display
#    coprocessor and its display_uart.c UART bridge were removed; display_power.c
#    shares the MIPI D-PHY LDO + I2C bus between the DSI panel and the camera.
#  - esp_wifi is replaced by esp_wifi_remote: the P4 has no native Wi-Fi/BLE;
#    both come from the on-board ESP32-C6 over ESP-Hosted (SDIO).
