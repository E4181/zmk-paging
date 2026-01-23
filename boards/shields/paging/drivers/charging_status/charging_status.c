#define DT_DRV_COMPAT zmk_charging_status

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(charging_status, LOG_LEVEL_INF);

/* =======================
 *  ZMK battery 访问（安全版）
 * ======================= */

/*
 * 不 include <zmk/battery.h>，避免 shield driver 头文件不可见问题。
 * 这里使用 extern + weak fallback 的方式。
 */

/* 正常情况下，ZMK app 层会提供这个符号 */
extern uint8_t zmk_battery_percent(void);

/*
 * 如果未来某次配置中 battery 子系统被裁剪，
 * 链接器找不到该符号，就会使用这个 weak 版本。
 */
__attribute__((weak))
uint8_t zmk_battery_percent(void)
{
    return 0;
}

/* =======================
 *  参数
 * ======================= */
#define DEBOUNCE_MS 50

/* =======================
 *  设备结构
 * ======================= */
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

/* =======================
 *  前向声明
 * ======================= */
static void chrg_isr(const struct device *dev,
                     struct gpio_callback *cb,
                     uint32_t pins);

static void debounce_timer_handler(struct k_timer *timer);
static void led_thread_fn(void *p1, void *p2, void *p3);

/* =======================
 *  充电判定逻辑（方案 A）
 * ======================= */

/*
 * 电池是否“真实存在”
 *
 * 在 ZMK 中：
 * - percent == 0 基本可以视为：
 *   - 无电池
 *   - ADC 未接
 *   - 电池异常
 */
static bool battery_present(void)
{
    return zmk_battery_percent() > 0;
}

/*
 * TP4056 CHRG + Battery 联合判断
 */
static bool charging_is_valid(int chrg_level)
{
    /* 没电池 → 不可能在充电 */
    if (!battery_present()) {
        return false;
    }

    /* TP4056: CHRG = LOW → 正在充电 */
    return chrg_level == 0;
}

/* =======================
 *  GPIO 中断
 * ======================= */
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

/* =======================
 *  消抖处理
 * ======================= */
static void debounce_timer_handler(struct k_timer *timer)
{
    struct charging_status_data *data =
        CONTAINER_OF(timer, struct charging_status_data, debounce_timer);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    const struct charging_status_config *cfg = dev->config;

    int chrg_level = gpio_pin_get_dt(&cfg->chrg);
    bool charging = charging_is_valid(chrg_level);

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

/* =======================
 *  LED 呼吸线程
 * ======================= */
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

        /* 简单稳定的“逻辑呼吸” */
        gpio_pin_set_dt(&cfg->led, 1);
        k_sleep(K_MSEC(150));
        gpio_pin_set_dt(&cfg->led, 0);
        k_sleep(K_MSEC(150));
    }
}

/* =======================
 *  初始化
 * ======================= */
static int charging_status_init(const struct device *dev)
{
    const struct charging_status_config *cfg = dev->config;
    struct charging_status_data *data = dev->data;
    int ret;

    if (!device_is_ready(cfg->chrg.port) ||
        !device_is_ready(cfg->led.port)) {
        return -ENODEV;
    }

    /* CHRG：只读 + 上拉（若你板上已有外部上拉，可去掉 GPIO_PULL_UP） */
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

    /* 初始采样 */
    int chrg_level = gpio_pin_get_dt(&cfg->chrg);
    data->charging = charging_is_valid(chrg_level);

    LOG_INF("Charging status driver initialized");
    LOG_INF("  CHRG raw level at boot: %d", chrg_level);
    LOG_INF("  Battery percent at boot: %u", zmk_battery_percent());
    LOG_INF("  Charging at boot: %s",
            data->charging ? "YES" : "NO");

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

/* =======================
 *  设备实例
 * ======================= */
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
