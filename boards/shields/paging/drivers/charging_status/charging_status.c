#define DT_DRV_COMPAT zmk_charging_status

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* ZMK battery API */
#include <zmk/battery.h>

LOG_MODULE_REGISTER(charging_status, LOG_LEVEL_INF);

/* ===== 参数 ===== */
#define DEBOUNCE_MS 50

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

/* ===== TP4056 + Battery 联合判定 ===== */
static bool is_battery_present(void)
{
    /*
     * ZMK battery subsystem:
     * - 0% 基本可以确定是“无电池 / 电池异常 / ADC 未接”
     */
    uint8_t percent = zmk_battery_percent();
    return percent > 0;
}

static bool is_charging_valid(int chrg_level)
{
    /*
     * TP4056:
     * CHRG = LOW  → “正在充电”（仅在电池存在时才有意义）
     */
    if (!is_battery_present()) {
        return false;
    }

    return chrg_level == 0;
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

/* ===== 消抖处理 ===== */
static void debounce_timer_handler(struct k_timer *timer)
{
    struct charging_status_data *data =
        CONTAINER_OF(timer, struct charging_status_data, debounce_timer);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    const struct charging_status_config *cfg = dev->config;

    int chrg_level = gpio_pin_get_dt(&cfg->chrg);
    bool charging = is_charging_valid(chrg_level);

    if (charging == data->charging) {
        return;
    }

    data->charging = charging;

    LOG_INF("Charging state update:");
    LOG_INF("  CHRG raw level: %d", chrg_level);
    LOG_INF("  Battery percent: %u", zmk_battery_percent());
    LOG_INF("  Charging: %s", charging ? "YES" : "NO");

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

    while (1) {
        while (!data->charging) {
            k_thread_suspend(k_current_get());
        }

        /* 简单呼吸（非 PWM，逻辑呼吸） */
        for (int i = 0; i < 10 && data->charging; i++) {
            gpio_pin_set_dt(&cfg->led, 1);
            k_sleep(K_MSEC(150));
            gpio_pin_set_dt(&cfg->led, 0);
            k_sleep(K_MSEC(150));
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

    /* CHRG: 只读 + 上拉（如果你有外部上拉，可去掉 GPIO_PULL_UP） */
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

    /* ===== 初始状态采样 ===== */
    int chrg_level = gpio_pin_get_dt(&cfg->chrg);
    data->charging = is_charging_valid(chrg_level);

    LOG_INF("Charging status driver initialized");
    LOG_INF("  CHRG raw level at boot: %d", chrg_level);
    LOG_INF("  Battery percent at boot: %u", zmk_battery_percent());
    LOG_INF("  Charging at boot: %s",
            data->charging ? "YES" : "NO");

    /* LED 线程 */
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
