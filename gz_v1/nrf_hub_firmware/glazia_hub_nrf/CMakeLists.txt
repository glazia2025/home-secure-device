cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(glazia_hub_nrf)

target_sources(app PRIVATE
    src/main.c
    src/uart_ipc.c
    src/thread_mgr.c
    src/event_relay.c
    src/led_indicator.c
)
