#pragma once
#include <stdbool.h>

extern volatile bool g_thread_ready;

/* Factory EUI64 as lowercase hex (e.g. "f4ce36acb7bb1907"), populated at join. Sent in every
 * event payload so the hub can identify the sensor by its real EUI64 (Thread mesh-local
 * addresses are randomized and can't be reversed to the EUI64). */
extern char g_sensor_eui64_hex[17];

void thread_join_init(void);   /* derives PSKd from EUI64, starts joiner task */
