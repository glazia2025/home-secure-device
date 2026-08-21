#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "sensors.h"
#include "event_tx.h"
#include "thread_join.h"
#include "led_indicator.h"
#include "factory_reset.h"

LOG_MODULE_REGISTER(glazia_sensor_nrf, LOG_LEVEL_INF);

int main(void)
{
    /* Before anything else: if the reset button is held at boot, wipe Thread credentials.
     * Runs before led_sensor_init() so it can drive the red LED without contention. */
    factory_reset_check();

    led_sensor_init();  /* 3× red blink at boot, starts LED task at JOINING */
    LOG_INF("glazia_sensor_nrf v1.0.0 starting");

    event_tx_init();
    sensors_init();
    thread_join_init();  /* derives PSKd from EUI64, initiates DTLS join */

    /* All activity is interrupt/callback driven */
    while (1) { k_sleep(K_FOREVER); }
    return 0;
}
