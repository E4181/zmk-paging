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

/* GPIO definitions */
static const struct gpio_dt_spec chrg_gpio =
    GPIO_DT_SPEC_GET(NODE, chrg_gpios);

static const struct gpio_dt_spec led_gpio =
    GPIO_DT_SPEC_GET(NODE, led_gpios);

/* Charging state */
static atomic_t charging_state;

/* GPIO interrupt callback */
static struct gpio_callback chrg_cb;

/* LED breathing work */
static struct k_work_delayable led_work;

/* breathing parameters */
static uint8_t led_duty;
static bool led_up;

#define LED_STEP_MS 30
#define LED_MAX     100

/* ========================================================= */
/* LED breathing worker (non-blocking)                       */
/* ========================================================= */
static void led_breathing_worker(struct k_work *work)
{
    if (!atomic_get(&charging_state)) {
        gpio_pin_set(led_gpio.port, led_gpio.pin, 0);
        return;
    }

    gpio_pin_set(led_gpio.port, led_gpio.pin, led_duty > 50);

    if (led_up) {
        if (++led_duty >= LED_MAX) {
            led_duty = LED_MAX;
            led_up = false;
        }
    } else {
        if (led_duty == 0) {
            led_up = true;
        } else {
            led_duty--;
        }
    }

    k_work_schedule(&led_work, K_MSEC(LED_STEP_MS));
}

/* ========================================================= */
/* CHRG interrupt: sample once, then RELEASE GPIO            */
/* ========================================================= */
static void chrg_gpio_isr(
    const struct device *port,
    struct gpio_callback *cb,
    uint32_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /* Read RAW physical level: TP4056 -> LOW = charging */
    int level = gpio_pin_get_raw(chrg_gpio.port, chrg_gpio.pin);
    if (level < 0) {
        return;
    }

    bool charging = (level == 0);
    bool prev = atomic_set(&charging_state, charging);

    /* CRITICAL: immediately disable interrupt to release CHRG */
    gpio_pin_interrupt_configure(
        chrg_gpio.port,
        chrg_gpio.pin,
        GPIO_INT_DISABLE
    );

    if (charging != prev) {
        LOG_INF("Charging state: %s",
                charging ? "CHARGING" : "NOT CHARGING");

        if (charging) {
            led_duty = 0;
            led_up = true;
            k_work_schedule(&led_work, K_NO_WAIT);
        } else {
            k_work_cancel_delayable(&led_work);
            gpio_pin_set(led_gpio.port, led_gpio.pin, 0);
        }
    }
}

/* ========================================================= */
/* Init                                                      */
/* ========================================================= */
static int charging_status_init(void)
{
    int ret;

    if (!device_is_ready(chrg_gpio.port) ||
        !device_is_ready(led_gpio.port)) {
        return -ENODEV;
    }

    /* CHRG: strict high-impedance input */
    ret = gpio_pin_configure(
        chrg_gpio.port,
        chrg_gpio.pin,
        GPIO_INPUT);
    if (ret) {
        return ret;
    }

    /* LED: output, default off */
    ret = gpio_pin_configure(
        led_gpio.port,
        led_gpio.pin,
        GPIO_OUTPUT_INACTIVE);
    if (ret) {
        return ret;
    }

    /* Initial one-time sample (no interrupt yet) */
    int level = gpio_pin_get_raw(chrg_gpio.port, chrg_gpio.pin);
    bool charging = (level == 0);
    atomic_set(&charging_state, charging);

    LOG_INF("Charging status init: %s",
            charging ? "CHARGING" : "NOT CHARGING");

    /* GPIO interrupt: enabled ONCE */
    gpio_init_callback(&chrg_cb, chrg_gpio_isr, BIT(chrg_gpio.pin));
    gpio_add_callback(chrg_gpio.port, &chrg_cb);

    gpio_pin_interrupt_configure(
        chrg_gpio.port,
        chrg_gpio.pin,
        GPIO_INT_EDGE_BOTH);

    /* LED work */
    k_work_init_delayable(&led_work, led_breathing_worker);

    if (charging) {
        led_duty = 0;
        led_up = true;
        k_work_schedule(&led_work, K_NO_WAIT);
    }

    return 0;
}

SYS_INIT(
    charging_status_init,
    POST_KERNEL,
    CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

/* ========================================================= */
/* Public API                                                */
/* ========================================================= */
bool charging_status_is_charging(void)
{
    return atomic_get(&charging_state);
}
