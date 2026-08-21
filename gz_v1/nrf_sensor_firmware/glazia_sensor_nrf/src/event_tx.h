#pragma once

void event_tx_init(void);
void event_tx_send(const char *event_str);  /* e.g. "door_open", "vibration" */
void event_tx_flush_cache(void);            /* called when Thread join succeeds */
