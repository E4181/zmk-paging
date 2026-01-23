#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

// 默认低电平表示充电中
#ifndef CONFIG_CHARGING_STATUS_ACTIVE_LOW
#define CONFIG_CHARGING_STATUS_ACTIVE_LOW 1
#endif

// 去抖动延迟（毫秒），可调整以平衡准确性和及时性
#define DEBOUNCE_MS 10

LOG_MODULE_REGISTER(charging_status, LOG_LEVEL_INF);

#define DT_DRV_COMPAT zmk_charging_status

struct charging_status_data {
    bool charging;
    struct gpio_callback gpio_cb;
    const struct sensor_trigger *trig_handler;
    sensor_trigger_handler_t handler;
    struct k_timer debounce_timer;
};

struct charging_status_cfg {
    struct gpio_dt_spec gpio;
};

static void debounce_handler(struct k_timer *timer) {
    struct charging_status_data *data = CONTAINER_OF(timer, struct charging_status_data, debounce_timer);
    const struct device *sensor = DEVICE_DT_GET(DT_DRV_INST(0));
    const struct charging_status_cfg *cfg = sensor->config;
    int level = gpio_pin_get_dt(&cfg->gpio);
    bool new_charging;

#if CONFIG_CHARGING_STATUS_ACTIVE_LOW
    new_charging = (level == 0);
#else
    new_charging = (level == 1);
#endif

    if (new_charging != data->charging) {
        data->charging = new_charging;
        LOG_INF("Charging status changed to: %s (debounced)", data->charging ? "Charging" : "Completed");

        if (data->handler) {
            data->handler(sensor, data->trig_handler);
        }
    }

    // 重新启用中断
    gpio_pin_interrupt_configure_dt(&cfg->gpio, GPIO_INT_EDGE_BOTH);
}

static void charging_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    struct charging_status_data *data = CONTAINER_OF(cb, struct charging_status_data, gpio_cb);
    const struct device *sensor = DEVICE_DT_GET(DT_DRV_INST(0));
    const struct charging_status_cfg *cfg = sensor->config;

    // 禁用中断以防止噪声触发
    gpio_pin_interrupt_configure_dt(&cfg->gpio, GPIO_INT_DISABLE);

    // 启动去抖动定时器
    k_timer_start(&data->debounce_timer, K_MSEC(DEBOUNCE_MS), K_NO_WAIT);
}

static int charging_status_init(const struct device *dev) {
    struct charging_status_data *data = dev->data;
    const struct charging_status_cfg *cfg = dev->config;
    int ret;

    if (!gpio_is_ready_dt(&cfg->gpio)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&cfg->gpio, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&cfg->gpio, GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_ERR("Failed to configure interrupt: %d", ret);
        return ret;
    }

    gpio_init_callback(&data->gpio_cb, charging_isr, BIT(cfg->gpio.pin));
    ret = gpio_add_callback(cfg->gpio.port, &data->gpio_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add callback: %d", ret);
        return ret;
    }

    // 初始化去抖动定时器
    k_timer_init(&data->debounce_timer, debounce_handler, NULL);

    // 初始读取并日志（无去抖动，因为是初始化）
    int level = gpio_pin_get_dt(&cfg->gpio);
#if CONFIG_CHARGING_STATUS_ACTIVE_LOW
    data->charging = (level == 0);
#else
    data->charging = (level == 1);
#endif
    LOG_INF("Initial charging status: %s", data->charging ? "Charging" : "Completed");

    return 0;
}

static int charging_status_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    if (chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    struct charging_status_data *data = dev->data;
    const struct charging_status_cfg *cfg = dev->config;
    int level = gpio_pin_get_dt(&cfg->gpio);

#if CONFIG_CHARGING_STATUS_ACTIVE_LOW
    data->charging = (level == 0);
#else
    data->charging = (level == 1);
#endif

    return 0;
}

static int charging_status_channel_get(const struct device *dev, enum sensor_channel chan, struct sensor_value *val) {
    struct charging_status_data *data = dev->data;

    if (chan != SENSOR_CHAN_ALL) {
        return -ENOTSUP;
    }

    val->val1 = data->charging ? 1 : 0;
    val->val2 = 0;

    return 0;
}

static int charging_status_trigger_set(const struct device *dev, const struct sensor_trigger *trig, sensor_trigger_handler_t handler) {
    struct charging_status_data *data = dev->data;

    if (trig->type != SENSOR_TRIG_DELTA) {
        return -ENOTSUP;
    }

    data->trig_handler = trig;
    data->handler = handler;

    return 0;
}

static const struct sensor_driver_api charging_status_api = {
    .sample_fetch = charging_status_sample_fetch,
    .channel_get = charging_status_channel_get,
    .trigger_set = charging_status_trigger_set,
};

#define CHARGING_STATUS_INST(inst)                                             \
    static struct charging_status_data charging_status_data_##inst;            \
    static const struct charging_status_cfg charging_status_cfg_##inst = {     \
        .gpio = GPIO_DT_SPEC_INST_GET(inst, gpios),                            \
    };                                                                         \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, charging_status_init, NULL,             \
                                 &charging_status_data_##inst,                 \
                                 &charging_status_cfg_##inst, POST_KERNEL,     \
                                 CONFIG_SENSOR_INIT_PRIORITY,                  \
                                 &charging_status_api);

DT_INST_FOREACH_STATUS_OKAY(CHARGING_STATUS_INST)