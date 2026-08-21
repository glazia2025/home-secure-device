#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/instance.h>
#include "factory_reset.h"

LOG_MODULE_REGISTER(glazia_sensor_nrf_reset, LOG_LEVEL_INF);

#define HOLD_MS 3000   /* button must stay pressed this long to trigger a wipe */

/* Factory-reset button: D1 / P0.03, active-low + internal pull-up (see board overlay). */
static const struct gpio_dt_spec s_btn = GPIO_DT_SPEC_GET(DT_ALIAS(resetbtn), gpios);
/* Red LED (led0). Driven directly here because factory_reset_check() runs before the LED task
 * starts, so there is no contention for the pin. */
static const struct gpio_dt_spec s_red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

void factory_reset_check(void)
{
    if (!gpio_is_ready_dt(&s_btn)) {
        LOG_WRN("reset button GPIO not ready — skipping factory-reset check");
        return;
    }
    gpio_pin_configure_dt(&s_btn, GPIO_INPUT);

    /* Not pressed at boot -> return immediately, no boot delay. */
    if (gpio_pin_get_dt(&s_btn) == 0) return;

    LOG_WRN("reset button held — keep holding %d s to wipe Thread credentials", HOLD_MS / 1000);

    bool have_led = gpio_is_ready_dt(&s_red);
    if (have_led) {
        gpio_pin_configure_dt(&s_red, GPIO_OUTPUT_INACTIVE);
        gpio_pin_set_dt(&s_red, 1);   /* solid red while confirming the hold */
    }

    for (int held = 0; held < HOLD_MS; held += 100) {
        if (gpio_pin_get_dt(&s_btn) == 0) {   /* released early — abort */
            if (have_led) gpio_pin_set_dt(&s_red, 0);
            LOG_INF("reset button released before %d s — not wiping", HOLD_MS / 1000);
            return;
        }
        k_msleep(100);
    }

    LOG_WRN("factory reset — erasing credentials & rebooting");
    struct openthread_context *ctx = openthread_get_default_context();
    otInstanceFactoryReset(ctx->instance);   /* erases persistent settings + reboots; no return */
}
