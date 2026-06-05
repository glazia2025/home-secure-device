#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef void (*door_lock_ack_sender_t)(const char *command_id,
                                       const char *status,
                                       const char *lock_state,
                                       const char *error);

typedef struct {
    char command_id[64];
    char mode[16];
    char action[8];
    uint32_t duration_ms;
} door_lock_command_t;

esp_err_t door_lock_init(void);
esp_err_t door_lock_start(door_lock_ack_sender_t ack_sender);
esp_err_t door_lock_enqueue(const door_lock_command_t *command);
