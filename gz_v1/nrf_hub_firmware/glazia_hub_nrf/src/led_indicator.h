#pragma once

typedef enum {
    LED_HUB_DETACHED,
    LED_HUB_NETWORK_UP,
    LED_HUB_COMMISSIONING,
} led_hub_state_t;

void led_hub_init(void);
void led_hub_set_state(led_hub_state_t s);
void led_hub_flash_joined(void);      /* 2× green burst */
void led_hub_flash_comm_failed(void); /* 3× red burst */
void led_hub_flash_rx(void);          /* blue: IPC packet received */
void led_hub_flash_ack_rdy(void);     /* blue → green: packet received then ACK sent */
