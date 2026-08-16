#pragma once
#include <stdint.h>

void thread_mgr_init(void);
void thread_mgr_form_network(void);
void thread_mgr_report_status(void);
void thread_mgr_commission(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s);
void thread_mgr_remove_joiner(const uint8_t eui64[8]);
