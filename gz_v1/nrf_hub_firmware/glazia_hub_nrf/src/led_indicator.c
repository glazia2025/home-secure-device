#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "led_indicator.h"

static const struct gpio_dt_spec s_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec s_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec s_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static volatile led_hub_state_t s_state = LED_HUB_DETACHED;
static atomic_t s_flash = ATOMIC_INIT(0);

#define FLASH_JOINED      1
#define FLASH_COMM_FAILED 2
#define FLASH_RX          3   /* blue: IPC packet received */
#define FLASH_ACK_RDY     4   /* blue then green: packet received → ACK (PONG) sent */

static void all_off(void)
{
    gpio_pin_set_dt(&s_red,   0);
    gpio_pin_set_dt(&s_green, 0);
    gpio_pin_set_dt(&s_blue,  0);
}

static void do_flash(atomic_val_t type)
{
    all_off();
    switch (type) {
    case FLASH_JOINED:
        for (int i = 0; i < 2; i++) {
            gpio_pin_set_dt(&s_green, 1); k_msleep(100);
            gpio_pin_set_dt(&s_green, 0); k_msleep(100);
        }
        break;
    case FLASH_RX:
        gpio_pin_set_dt(&s_blue, 1); k_msleep(150);
        gpio_pin_set_dt(&s_blue, 0);
        break;
    case FLASH_ACK_RDY:
        gpio_pin_set_dt(&s_blue,  1); k_msleep(150);
        gpio_pin_set_dt(&s_blue,  0);
        gpio_pin_set_dt(&s_green, 1); k_msleep(150);
        gpio_pin_set_dt(&s_green, 0);
        break;
    case FLASH_COMM_FAILED:
    default:
        for (int i = 0; i < 3; i++) {
            gpio_pin_set_dt(&s_red, 1); k_msleep(100);
            gpio_pin_set_dt(&s_red, 0); k_msleep(100);
        }
        break;
    }
}

static K_THREAD_STACK_DEFINE(s_stack, 512);
static struct k_thread s_thread;

static void led_task(void *a, void *b, void *c)
{
    int tick = 0;

    while (1) {
        atomic_val_t flash = atomic_set(&s_flash, 0);
        if (flash) {
            do_flash(flash);
            tick = 0;
            continue;
        }

        switch (s_state) {
        case LED_HUB_DETACHED:
            /* Red 500 ms on/off: 10 ticks on, 10 ticks off at 50 ms/tick */
            gpio_pin_set_dt(&s_green, 0);
            gpio_pin_set_dt(&s_blue,  0);
            gpio_pin_set_dt(&s_red, (tick % 20) < 10 ? 1 : 0);
            break;
        case LED_HUB_NETWORK_UP:
            gpio_pin_set_dt(&s_red,   0);
            gpio_pin_set_dt(&s_blue,  0);
            gpio_pin_set_dt(&s_green, 1);
            break;
        case LED_HUB_COMMISSIONING:
            /* Green solid + blue 250 ms on/off: 5 ticks on, 5 ticks off */
            gpio_pin_set_dt(&s_red,   0);
            gpio_pin_set_dt(&s_green, 1);
            gpio_pin_set_dt(&s_blue, (tick % 10) < 5 ? 1 : 0);
            break;
        }

        tick++;
        k_msleep(50);
    }
}

void led_hub_init(void)
{
    if (!gpio_is_ready_dt(&s_red) ||
        !gpio_is_ready_dt(&s_green) ||
        !gpio_is_ready_dt(&s_blue)) {
        return;
    }

    gpio_pin_configure_dt(&s_red,   GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&s_blue,  GPIO_OUTPUT_INACTIVE);

    /* Boot blink: 3× red = main() reached */
    for (int i = 0; i < 3; i++) {
        gpio_pin_set_dt(&s_red, 1); k_msleep(200);
        gpio_pin_set_dt(&s_red, 0); k_msleep(200);
    }

    k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack),
                    led_task, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
    k_thread_name_set(&s_thread, "led_hub");
}

void led_hub_set_state(led_hub_state_t s)  { s_state = s; }
void led_hub_flash_joined(void)            { atomic_set(&s_flash, FLASH_JOINED); }
void led_hub_flash_comm_failed(void)       { atomic_set(&s_flash, FLASH_COMM_FAILED); }
void led_hub_flash_rx(void)                { atomic_set(&s_flash, FLASH_RX); }
void led_hub_flash_ack_rdy(void)           { atomic_set(&s_flash, FLASH_ACK_RDY); }
