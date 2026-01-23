#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include "charging_status.h"

LOG_MODULE_REGISTER(charging_status, CONFIG_CHARGING_STATUS_LOG_LEVEL);

#define DT_DRV_COMPAT custom_charging_status
#define NODE DT_INST(0, custom_charging_status)

#if !DT_NODE_HAS_STATUS(NODE, okay)
#error "charging_status devicetree node missing"
#endif

static const struct gpio_dt_spec chrg_gpio =
    GPIO_DT_SPEC_GET(NODE, chrg_gpios);

static const struct gpio_dt_spec led_gpio =
    GPIO_DT_SPEC_GET(NODE, led_gpios);

static struct gpio_callback chrg_cb;
static atomic_t charging_state;

/* ===== LED breathing control ===== */

static struct k_work_delayable led_work;
static uint8_t led_level;
static bool led_fade_up;

#define LED_STEP      5
#define LED_PERIOD_MS 20
#define LED_MAX       100

static void led_breathing_worker(struct k_work *work)
{
    if (!atomic_get(&charging_state)) {
        gpio_pin_set_dt(&led_gpio, 0);
        return;
    }

    gpio_pin_set_dt(&led_gpio, 1);
    k_busy_wait(led_level * 50);
    gpio_pin_set_dt(&led_gpio, 0);
    k_busy_wait((LED_MAX - led_level) * 50);

    if (led_fade_up) {
        led_level += LED_STEP;
        if (led_level >= LED_MAX) {
            led_level = LED_MAX;
            led_fade_up = false;
        }
    } else {
        if (led_level <= LED_STEP) {
            led_level = 0;
            led_fade_up = true;
        } else {
            led_level -= LED_STEP;
        }
    }

    k_work_schedule(&led_work, K_MSEC(LED_PERIOD_MS));
}

/* ===== CHRG interrupt ===== */

static void chrg_gpio_isr(
    const struct device *port,
    struct gpio_callback *cb,
    uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    int level = gpio_pin_get_dt(&chrg_gpio);
    if (level < 0) {
        return;
    }

    bool charging = (level == 0);
    bool prev = atomic_set(&charging_state, charging);

    if (charging != prev) {
        LOG_INF("Charging state changed: %s",
                charging ? "CHARGING" : "NOT CHARGING");

        if (charging) {
            led_level = 0;
            led_fade_up = true;
            k_work_schedule(&led_work, K_NO_WAIT);
        } else {
            k_work_cancel_delayable(&led_work);
            gpio_pin_set_dt(&led_gpio, 0);
        }
    }
}

/* ===== Init ===== */

static int charging_status_init(void)
{
    int ret;

    if (!device_is_ready(chrg_gpio.port) ||
        !device_is_ready(led_gpio.port)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&chrg_gpio, GPIO_INPUT);
    if (ret) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&led_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret) {
        return ret;
    }

    int level = gpio_pin_get_dt(&chrg_gpio);
    atomic_set(&charging_state, level == 0);

    LOG_INF("Charging status init: %s",
            level == 0 ? "CHARGING" : "NOT CHARGING");

    gpio_init_callback(&chrg_cb, chrg_gpio_isr, BIT(chrg_gpio.pin));
    gpio_add_callback(chrg_gpio.port, &chrg_cb);

    gpio_pin_interrupt_configure_dt(
        &chrg_gpio, GPIO_INT_EDGE_BOTH);

    k_work_init_delayable(&led_work, led_breathing_worker);

    if (atomic_get(&charging_state)) {
        k_work_schedule(&led_work, K_NO_WAIT);
    }

    return 0;
}

SYS_INIT(
    charging_status_init,
    POST_KERNEL,
    CONFIG_KERNEL_INIT_PRIORITY_DEVICE
);

bool charging_status_is_charging(void)
{
    return atomic_get(&charging_state);
}
