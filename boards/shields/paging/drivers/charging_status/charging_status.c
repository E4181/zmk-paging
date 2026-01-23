BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 0,
             "No charging_status device instances found");


#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT zmk_charging_status

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(charging_status, LOG_LEVEL_INF);

/* ===== 可调参数 ===== */
#define DEBOUNCE_MS     50
#define BREATH_PERIOD  2000
#define BREATH_STEP    20

/* ===== 设备结构 ===== */
struct charging_status_config {
    struct gpio_dt_spec chrg;
    struct gpio_dt_spec led;
};

struct charging_status_data {
    struct gpio_callback chrg_cb;
    struct k_timer debounce_timer;
    struct k_thread led_thread;

    bool charging;
};

/* ===== 前向声明 ===== */
static void chrg_isr(const struct device *dev,
                     struct gpio_callback *cb,
                     uint32_t pins);

static void debounce_timer_handler(struct k_timer *timer);
static void led_thread_fn(void *p1, void *p2, void *p3);

/* ===== 工具函数 ===== */
static inline bool tp4056_is_charging(int raw_level)
{
    /* TP4056:
     * CHRG = LOW  -> charging
     * CHRG = HIGH -> done / idle
     */
    return raw_level == 0;
}

/* ===== 中断 ===== */
static void chrg_isr(const struct device *dev,
                     struct gpio_callback *cb,
                     uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(pins);

    struct charging_status_data *data =
        CONTAINER_OF(cb, struct charging_status_data, chrg_cb);

    k_timer_start(&data->debounce_timer,
                  K_MSEC(DEBOUNCE_MS),
                  K_NO_WAIT);
}

/* ===== 消抖 ===== */
static void debounce_timer_handler(struct k_timer *timer)
{
    struct charging_status_data *data =
        CONTAINER_OF(timer, struct charging_status_data, debounce_timer);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    const struct charging_status_config *cfg = dev->config;

    int level = gpio_pin_get_dt(&cfg->chrg);
    bool charging = tp4056_is_charging(level);

    if (charging == data->charging) {
        return;
    }

    data->charging = charging;

    LOG_INF("Charging state: %s",
            charging ? "CHARGING" : "NOT CHARGING");

    if (charging) {
        k_thread_resume(&data->led_thread);
    } else {
        gpio_pin_set_dt(&cfg->led, 0);
        k_thread_suspend(&data->led_thread);
    }
}

/* ===== LED 呼吸线程 ===== */
K_THREAD_STACK_DEFINE(led_stack, 512);

static void led_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    const struct device *dev = p1;
    const struct charging_status_config *cfg = dev->config;
    struct charging_status_data *data = dev->data;

    uint32_t step_delay = BREATH_PERIOD / (2 * (100 / BREATH_STEP));

    while (1) {
        while (!data->charging) {
            k_thread_suspend(k_current_get());
        }

        for (int i = 0; i <= 100 && data->charging; i += BREATH_STEP) {
            gpio_pin_set_dt(&cfg->led, 1);
            k_sleep(K_MSEC(step_delay));
            gpio_pin_set_dt(&cfg->led, 0);
            k_sleep(K_MSEC(step_delay));
        }

        for (int i = 100; i >= 0 && data->charging; i -= BREATH_STEP) {
            gpio_pin_set_dt(&cfg->led, 1);
            k_sleep(K_MSEC(step_delay));
            gpio_pin_set_dt(&cfg->led, 0);
            k_sleep(K_MSEC(step_delay));
        }
    }
}

/* ===== 初始化 ===== */
static int charging_status_init(const struct device *dev)
{
    const struct charging_status_config *cfg = dev->config;
    struct charging_status_data *data = dev->data;
    int ret;

    if (!device_is_ready(cfg->chrg.port) ||
        !device_is_ready(cfg->led.port)) {
        return -ENODEV;
    }

    /* CHRG: 永远只读 */
    ret = gpio_pin_configure_dt(&cfg->chrg,
                                GPIO_INPUT | GPIO_PULL_UP);
    if (ret) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&cfg->led, GPIO_OUTPUT_INACTIVE);
    if (ret) {
        return ret;
    }

    gpio_init_callback(&data->chrg_cb,
                       chrg_isr,
                       BIT(cfg->chrg.pin));

    gpio_add_callback(cfg->chrg.port, &data->chrg_cb);

    ret = gpio_pin_interrupt_configure_dt(&cfg->chrg,
                                          GPIO_INT_EDGE_BOTH);
    if (ret) {
        return ret;
    }

    k_timer_init(&data->debounce_timer,
                 debounce_timer_handler,
                 NULL);

    /* 初始状态 */
    int level = gpio_pin_get_dt(&cfg->chrg);
    data->charging = tp4056_is_charging(level);

    k_thread_create(&data->led_thread,
                    led_stack,
                    K_THREAD_STACK_SIZEOF(led_stack),
                    led_thread_fn,
                    (void *)dev,
                    NULL,
                    NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO,
                    0,
                    K_NO_WAIT);

    if (!data->charging) {
        k_thread_suspend(&data->led_thread);
    }

    LOG_INF("Charging status driver initialized");
    return 0;
}

/* ===== 设备实例 ===== */
static struct charging_status_data charging_status_data;

static const struct charging_status_config charging_status_config = {
    .chrg = GPIO_DT_SPEC_INST_GET(0, chrg_gpios),
    .led  = GPIO_DT_SPEC_INST_GET(0, led_gpios),
};

DEVICE_DT_INST_DEFINE(0,
                      charging_status_init,
                      NULL,
                      &charging_status_data,
                      &charging_status_config,
                      POST_KERNEL,
                      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
                      NULL);
