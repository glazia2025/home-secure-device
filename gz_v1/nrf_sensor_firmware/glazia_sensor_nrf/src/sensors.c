#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "sensors.h"
#include "event_tx.h"

LOG_MODULE_REGISTER(glazia_sensor_nrf_sensors, LOG_LEVEL_INF);

#define DEBOUNCE_MS     50
#define VIB_COOLDOWN_MS 2000

/* DTS aliases from xiao_ble.overlay: sensor0=reed, sensor1=vibration */
static const struct gpio_dt_spec s_reed = GPIO_DT_SPEC_GET(DT_ALIAS(sensor0), gpios);
static const struct gpio_dt_spec s_vib  = GPIO_DT_SPEC_GET(DT_ALIAS(sensor1), gpios);

static struct gpio_callback s_reed_cb;
static struct gpio_callback s_vib_cb;

static K_SEM_DEFINE(s_reed_sem, 0, 8);
static K_SEM_DEFINE(s_vib_sem,  0, 8);

static void reed_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_sem_give(&s_reed_sem);
}

static void vib_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_sem_give(&s_vib_sem);
}

/* ── Reed task ───────────────────────────────────────────────────────────── */
static K_THREAD_STACK_DEFINE(s_reed_stack, 1024);
static struct k_thread s_reed_thread;

static void reed_task(void *a, void *b, void *c)
{
    int last = gpio_pin_get_dt(&s_reed);

    while (1) {
        k_sem_take(&s_reed_sem, K_FOREVER);
        k_sleep(K_MSEC(DEBOUNCE_MS));
        int lvl = gpio_pin_get_dt(&s_reed);
        if (lvl == last) continue;
        last = lvl;
        /* active-low: 0 = magnet present = door closed */
        const char *evt = lvl == 0 ? "door_close" : "door_open";
        LOG_INF("%s", evt);
        event_tx_send(evt);
    }
}

/* ── Vibration task ──────────────────────────────────────────────────────── */
static K_THREAD_STACK_DEFINE(s_vib_stack, 1024);
static struct k_thread s_vib_thread;

static void vib_task(void *a, void *b, void *c)
{
    while (1) {
        k_sem_take(&s_vib_sem, K_FOREVER);
        LOG_INF("vibration");
        event_tx_send("vibration");

        /* drain ISR noise during cooldown */
        k_timepoint_t deadline = sys_timepoint_calc(K_MSEC(VIB_COOLDOWN_MS));
        while (!sys_timepoint_expired(deadline))
            k_sem_take(&s_vib_sem, K_MSEC(50));
    }
}

void sensors_init(void)
{
    if (device_is_ready(s_reed.port)) {
        gpio_pin_configure_dt(&s_reed, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&s_reed, GPIO_INT_EDGE_BOTH);
        gpio_init_callback(&s_reed_cb, reed_isr, BIT(s_reed.pin));
        gpio_add_callback(s_reed.port, &s_reed_cb);
        LOG_INF("reed on GPIO%d", s_reed.pin);
    } else {
        LOG_ERR("reed GPIO not ready");
    }

    if (device_is_ready(s_vib.port)) {
        gpio_pin_configure_dt(&s_vib, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&s_vib, GPIO_INT_EDGE_RISING);
        gpio_init_callback(&s_vib_cb, vib_isr, BIT(s_vib.pin));
        gpio_add_callback(s_vib.port, &s_vib_cb);
        LOG_INF("vibration on GPIO%d", s_vib.pin);
    } else {
        LOG_ERR("vibration GPIO not ready");
    }

    k_thread_create(&s_reed_thread, s_reed_stack, K_THREAD_STACK_SIZEOF(s_reed_stack),
                    reed_task, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&s_reed_thread, "reed");

    k_thread_create(&s_vib_thread, s_vib_stack, K_THREAD_STACK_SIZEOF(s_vib_stack),
                    vib_task, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&s_vib_thread, "vib");
}
