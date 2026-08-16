#pragma once
#include <stdint.h>
#include <stdbool.h>

extern volatile bool g_thread_net_ready;

void nrf_thread_preinit(void);
void nrf_thread_on_wifi_ready(void);
void nrf_thread_commission_sensor(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s);
void nrf_thread_delete_sensor(const uint8_t eui64[8]);
