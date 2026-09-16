#pragma once
#include <stdint.h>

void thread_mgr_init(void);
void thread_mgr_form_network(void);
void thread_mgr_report_status(void);
void thread_mgr_commission(const uint8_t eui64[8], const char *pskd, uint16_t timeout_s);
void thread_mgr_remove_joiner(const uint8_t eui64[8]);

/* Read the current Thread child table into `out` (N × 8-byte MLE ext address); returns N. */
int  thread_mgr_child_snapshot(uint8_t *out, int max_children);
/* Snapshot the child table and send it to the ESP as IPC_EVT_CHILD_LIST (reply to CMD_CHILD_POLL). */
void thread_mgr_report_children(void);
