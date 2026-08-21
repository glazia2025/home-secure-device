cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(glazia_sensor_nrf)

target_sources(app PRIVATE
    src/main.c
    src/sensors.c
    src/event_tx.c
    src/thread_join.c
    src/led_indicator.c
    src/factory_reset.c
)
