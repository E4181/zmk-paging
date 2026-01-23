 #include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "charging_status.h"

#define DT_DRV_COMPAT custom_charging_status

/* Devicetree node */
#define CHARGING_NODE DT_INST(0, custom_charging_status)

#if !DT_NODE_HAS_STATUS(CHARGING_NODE, okay)
#error "charging_status devicetree node is missing or disabled"
#endif

/* GPIO specification */
static const struct gpio_dt_spec chrg_gpio =
    GPIO_DT_SPEC_GET(CHARGING_NODE, chrg_gpios);

/* Charging state (atomic, interrupt-safe) */
static atomic_t charging_state;

/* GPIO callback */
static struct gpio_callback chrg_cb;

/* Interrupt handler */
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

    /* ACTIVE_LOW: low level means charging */
    atomic_set(&charging_state, level == 0);
}

static int charging_status_init(void)
{
    int ret;

    if (!device_is_ready(chrg_gpio.port)) {
        return -ENODEV;
    }

    /* Input only, NO internal pull */
    ret = gpio_pin_configure_dt(&chrg_gpio, GPIO_INPUT);
    if (ret) {
        return ret;
    }

    /* Initial state */
    int level = gpio_pin_get_dt(&chrg_gpio);
    if (level >= 0) {
        atomic_set(&charging_state, level == 0);
    }

    /* Configure interrupt: both edges */
    ret = gpio_pin_interrupt_configure_dt(
        &chrg_gpio,
        GPIO_INT_EDGE_BOTH);
    if (ret) {
        return ret;
    }

    gpio_init_callback(
        &chrg_cb,
        chrg_gpio_isr,
        BIT(chrg_gpio.pin));

    ret = gpio_add_callback(chrg_gpio.port, &chrg_cb);
    if (ret) {
        return ret;
    }

    return 0;
}

/* Init at POST_KERNEL to ensure GPIO ready */
SYS_INIT(charging_status_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

/* Public API */
bool charging_status_is_charging(void)
{
    return atomic_get(&charging_state);
}
