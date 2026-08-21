#pragma once

typedef enum {
    LED_SENSOR_JOINING,
    LED_SENSOR_READY,
} led_sensor_state_t;

void led_sensor_init(void);
void led_sensor_set_state(led_sensor_state_t s);
void led_sensor_flash_sent(void);   /* green 100 ms */
void led_sensor_flash_cached(void); /* red 100 ms */
