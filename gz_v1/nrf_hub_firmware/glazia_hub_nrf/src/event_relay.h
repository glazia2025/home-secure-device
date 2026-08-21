#pragma once

#include <stdint.h>

void event_relay_init(void);

/* Multicast a "leave" command addressed (by EUI64) to a single sensor so it factory-resets and
 * drops off the Thread network. Sent when the hub deletes the sensor. */
void event_relay_send_leave(const uint8_t eui64[8]);
