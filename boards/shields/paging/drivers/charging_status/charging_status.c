#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

// 默认低电平表示充电中
#ifndef CONFIG_CHARGING_STATUS_ACTIVE_LOW
#define CONFIG_CHARGING_STATUS_ACTIVE_LOW 1
#endif

// 去抖动延迟（毫秒）
#define DEBOUNCE_MS 10

// 呼吸灯参数：周期2.56ms (~390Hz)，分辨率256步，每步10us
#define PWM_PERIOD_US 2560
#define PWM_STEP_US 10
#define BREATH_STEP_MS 5  // 每步渐变延时，控制呼吸速度

// 呼吸线程栈大小
#define BREATH_STACK_SIZE 512

LOG_MODULE_REGISTER(charging_status, LOG_LEVEL_DBG);

#define DT_DRV_COMPAT zmk_charging_status

struct charging_status_data {
    bool charging;
    struct gpio_callback gpio_cb;
    const struct sensor_trigger *trig_handler;
    sensor_trigger_handler_t handler;
    struct k_timer debounce_timer;
    struct k_thread breath_thread_data;
    k_tid_t breath_tid;
    K_THREAD_STACK_MEMBER(breath_stack, BREATH_STACK_SIZE);
};

struct charging_status_cfg {
    struct gpio_dt_spec gpio;  // CHRG GPIO
    struct gpio_dt_spec led;   // LED GPIO
};

static void breath_led_thread(void *p1, void *p2, void *p3) {
    const struct device *dev = (const struct device *)p1;
    const struct charging_status_cfg *cfg = dev->config;

    while (1) {
        // 渐亮
        for (int brightness = 0; brightness <= 255; brightness++) {
            int on_time = brightness * PWM_STEP_US;
            gpio_pin_set_dt(&cfg->led, 1);  // 高电平点亮（假设active-high；若low，翻转）
            k_busy_wait(on_time);
            gpio_pin_set_dt(&cfg->led, 0);
            k_busy_wait(PWM_PERIOD_US - on_time);
            k_msleep(BREATH_STEP_MS);  // 渐变速度
        }
        // 渐暗
        for (int brightness = 255; brightness >= 0; brightness--) {
            int on_time = brightness * PWM_STEP_US;
            gpio_pin_set_dt(&cfg->led, 1);
            k_busy_wait(on_time);
            gpio_pin_set_dt(&cfg->led, 0);
            k_busy_wait(PWM_PERIOD_US - on_time);
            k_msleep(BREATH_STEP_MS);
        }
    }
}

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

    LOG_DBG("Debounced CHRG level: %d (new_charging: %d)", level, new_charging);

    if (new_charging != data->charging) {
        data->charging = new_charging;
        LOG_INF("Charging status changed to: %s (debounced)", data->charging ? "Charging" : "Completed");

        // LED控制：充电时启动呼吸，停止时关闭LED
        if (data->charging) {
            k_thread_resume(data->breath_tid);
        } else {
            k_thread_suspend(data->breath_tid);
            gpio_pin_set_dt(&cfg->led, 0);  // 关闭LED
        }

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
    int level = gpio_pin_get_dt(&cfg->gpio);

    LOG_DBG("ISR triggered, raw CHRG level: %d", level);

    // 禁用中断以防止噪声触发
    gpio_pin_interrupt_configure_dt(&cfg->gpio, GPIO_INT_DISABLE);

    // 启动去抖动定时器
    k_timer_start(&data->debounce_timer, K_MSEC(DEBOUNCE_MS), K_NO_WAIT);
}

static int charging_status_init(const struct device *dev) {
    struct charging_status_data *data = dev->data;
    const struct charging_status_cfg *cfg = dev->config;
    int ret;

    // 配置CHRG GPIO为纯输入模式（无内部上拉，依赖外部上拉）
    if (!gpio_is_ready_dt(&cfg->gpio)) {
        LOG_ERR("CHRG GPIO device not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&cfg->gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure CHRG GPIO: %d", ret);
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

    // 配置LED GPIO（如果定义）
    if (gpio_is_ready_dt(&cfg->led)) {
        ret = gpio_pin_configure_dt(&cfg->led, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure LED GPIO: %d", ret);
            return ret;
        }
    } else {
        LOG_WRN("LED GPIO not defined, skipping LED init");
    }

    // 初始化去抖动定时器
    k_timer_init(&data->debounce_timer, debounce_handler, NULL);

    // 创建呼吸线程（初始挂起）
    data->breath_tid = k_thread_create(&data->breath_thread_data, data->breath_stack,
                                       K_THREAD_STACK_SIZEOF(data->breath_stack),
                                       breath_led_thread, (void *)dev, NULL, NULL,
                                       K_PRIO_COOP(7), 0, K_NO_WAIT);
    k_thread_suspend(data->breath_tid);

    // 初始读取并日志（重复读取两次验证稳定性）
    int level1 = gpio_pin_get_dt(&cfg->gpio);
    k_msleep(5);  // 短延时验证
    int level2 = gpio_pin_get_dt(&cfg->gpio);
    int level = (level1 == level2) ? level1 : -1;  // 不一致标记错误
#if CONFIG_CHARGING_STATUS_ACTIVE_LOW
    data->charging = (level == 0);
#else
    data->charging = (level == 1);
#endif
    LOG_INF("Initial charging status: %s (level1: %d, level2: %d)", data->charging ? "Charging" : "Completed", level1, level2);

    // 初始LED状态
    if (data->charging) {
        k_thread_resume(data->breath_tid);
    } else {
        gpio_pin_set_dt(&cfg->led, 0);
    }

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

    LOG_DBG("Sample fetch CHRG level: %d", level);

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
        .led = GPIO_DT_SPEC_INST_GET_OR(inst, led_gpios, {0}),                 \
    };                                                                         \
    SENSOR_DEVICE_DT_INST_DEFINE(inst, charging_status_init, NULL,             \
                                 &charging_status_data_##inst,                 \
                                 &charging_status_cfg_##inst, POST_KERNEL,     \
                                 CONFIG_SENSOR_INIT_PRIORITY,                  \
                                 &charging_status_api);

DT_INST_FOREACH_STATUS_OKAY(CHARGING_STATUS_INST)