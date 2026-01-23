/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * Charging Status Driver for TP4056 with LED indication
 * Uses interrupt mode for real-time charging status detection
 */

#define DT_DRV_COMPAT zmk_charging_status

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <math.h>

/* 直接在驱动文件中定义Kconfig宏 */
#ifndef CONFIG_CHARGING_STATUS_LOG_LEVEL
#define CONFIG_CHARGING_STATUS_LOG_LEVEL 4  /* DEBUG级别 */
#endif

#ifndef CONFIG_CHARGING_STATUS_INIT_PRIORITY
#define CONFIG_CHARGING_STATUS_INIT_PRIORITY 70
#endif

#ifndef CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE
#define CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE 512
#endif

#ifndef CONFIG_CHARGING_STATUS_THREAD_PRIORITY
#define CONFIG_CHARGING_STATUS_THREAD_PRIORITY 10
#endif

#ifndef CONFIG_CHARGING_STATUS_DEBOUNCE_MS
#define CONFIG_CHARGING_STATUS_DEBOUNCE_MS 100  /* 防抖动时间 */
#endif

#ifndef CONFIG_CHARGING_STATUS_MIN_STABLE_TIME_MS
#define CONFIG_CHARGING_STATUS_MIN_STABLE_TIME_MS 500  /* 最小稳定时间 */
#endif

#ifndef CONFIG_CHARGING_STATUS_LED_ENABLE
#define CONFIG_CHARGING_STATUS_LED_ENABLE 1  /* 启用LED指示 */
#endif

#ifndef CONFIG_CHARGING_STATUS_LED_USE_PWM
#define CONFIG_CHARGING_STATUS_LED_USE_PWM 0  /* 默认使用GPIO模式 */
#endif

#ifndef CONFIG_CHARGING_STATUS_LED_BREATHE_PERIOD_MS
#define CONFIG_CHARGING_STATUS_LED_BREATHE_PERIOD_MS 2000  /* 呼吸周期 */
#endif

#ifndef CONFIG_CHARGING_STATUS_LED_MAX_BRIGHTNESS
#define CONFIG_CHARGING_STATUS_LED_MAX_BRIGHTNESS 200  /* 最大亮度 */
#endif

LOG_MODULE_REGISTER(charging_status, CONFIG_CHARGING_STATUS_LOG_LEVEL);

/* 驱动数据结构 */
struct charging_status_data {
    /* GPIO回调结构 */
    struct gpio_callback chrg_cb;
    
    /* 当前充电状态 - 原子操作保证线程安全 */
    atomic_t charging;
    
    /* 最后一次状态变化的时间戳 */
    int64_t last_change_time;
    
    /* 防抖动工作 */
    struct k_work_delayable debounce_work;
    
    /* 监控线程 */
    struct k_thread monitor_thread;
    
    /* 线程栈 */
    k_thread_stack_t *thread_stack;
    
    /* 状态更新回调 */
    void (*user_callback)(bool is_charging, void *user_data);
    void *user_callback_data;
    
    /* 配置引用 */
    const struct charging_status_config *config;
    
    /* 设备指针 */
    const struct device *dev;
    
    /* 状态变化计数 */
    atomic_t change_count;
    
    /* 最后一次中断时间 */
    int64_t last_interrupt_time;
    
    /* 中断计数 */
    atomic_t interrupt_count;
    
    /* 最后一次稳定的状态 */
    int last_stable_state;
    
    /* 状态稳定开始时间 */
    int64_t stable_since;
    
    /* 硬件故障标志 */
    atomic_t hardware_fault;
    
    /* LED控制相关 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
    /* LED控制线程 */
    struct k_thread led_thread;
    k_thread_stack_t *led_thread_stack;
    
    /* LED定时器（GPIO模式） */
    struct k_timer led_timer;
    
    /* LED状态 */
    atomic_t led_enabled;
    atomic_t led_active;
    atomic_t breathe_active;
    
    /* 亮度相关 */
    uint8_t current_brightness;
    uint8_t max_brightness;
    
    /* 呼吸效果参数 */
    uint32_t breathe_period_ms;
    uint32_t breathe_step_ms;
    
    /* 闪烁间隔（GPIO模式） */
    uint32_t blink_interval_ms;
#endif
};

/* 驱动配置结构 */
struct charging_status_config {
    /* CHRG GPIO规格 */
    struct gpio_dt_spec chrg_gpio;
    
    /* 轮询间隔 (毫秒) */
    uint32_t status_interval_ms;
    
    /* LED配置 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
#if CONFIG_CHARGING_STATUS_LED_USE_PWM
    struct pwm_dt_spec led_pwm;
#else
    struct gpio_dt_spec led_gpio;
#endif
    uint32_t breathe_period_ms;
    uint32_t blink_interval_ms;
    uint8_t max_brightness;
#endif
    
    /* 设备实例编号 */
    uint8_t instance;
};

/* 前向声明 */
static void charging_status_debounce_work(struct k_work *work);
static void charging_status_monitor_thread(void *p1, void *p2, void *p3);

#if CONFIG_CHARGING_STATUS_LED_ENABLE
static void charging_status_led_thread(void *p1, void *p2, void *p3);
static void charging_status_led_timer_handler(struct k_timer *timer);
static void update_led_state(const struct device *dev, bool enable);
static void charging_status_led_init(const struct device *dev);
#endif

/* 读取稳定的GPIO状态（多次采样取多数） */
static int read_stable_gpio_state(const struct gpio_dt_spec *gpio)
{
    int samples[5];
    int low_count = 0, high_count = 0;
    
    for (int i = 0; i < 5; i++) {
        int state = gpio_pin_get_dt(gpio);
        if (state < 0) {
            return state;  /* 读取错误 */
        }
        samples[i] = state;
        if (state == 0) {
            low_count++;
        } else {
            high_count++;
        }
        k_busy_wait(1000);  /* 等待1ms */
    }
    
    /* 返回多数状态 */
    return (low_count > high_count) ? 0 : 1;
}

/* GPIO中断回调 */
static void charging_status_gpio_callback(const struct device *dev,
                                         struct gpio_callback *cb,
                                         uint32_t pins)
{
    struct charging_status_data *data = 
        CONTAINER_OF(cb, struct charging_status_data, chrg_cb);
    
    atomic_inc(&data->interrupt_count);
    data->last_interrupt_time = k_uptime_get();
    
    LOG_DBG("GPIO interrupt triggered on pin %s.%d, pins: 0x%08x",
            data->config->chrg_gpio.port->name, 
            data->config->chrg_gpio.pin,
            pins);
    
    /* 取消可能未完成的工作，然后重新调度（防抖动） */
    k_work_cancel_delayable(&data->debounce_work);
    k_work_schedule(&data->debounce_work, K_MSEC(CONFIG_CHARGING_STATUS_DEBOUNCE_MS));
}

/* 计算呼吸效果亮度曲线 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE && CONFIG_CHARGING_STATUS_LED_USE_PWM
static uint32_t calculate_breathe_brightness(uint32_t time_ms, uint32_t period_ms, uint8_t max_brightness)
{
    /* 使用正弦波计算呼吸效果 */
    double angle = (2.0 * M_PI * time_ms) / period_ms;
    double sine_value = sin(angle);
    
    /* 将正弦值从[-1, 1]映射到[0, max_brightness] */
    double normalized = (sine_value + 1.0) / 2.0;  /* 映射到[0, 1] */
    uint32_t brightness = (uint32_t)(normalized * normalized * max_brightness);
    
    return brightness;
}

/* PWM模式：设置LED亮度 */
static int set_led_brightness_pwm(const struct device *dev, uint8_t brightness)
{
    const struct charging_status_config *config = dev->config;
    struct charging_status_data *data = dev->data;
    
    if (!device_is_ready(config->led_pwm.dev)) {
        LOG_ERR("PWM device not ready");
        return -ENODEV;
    }
    
    /* 计算占空比 */
    uint32_t period_ns = config->led_pwm.period;
    uint32_t pulse_ns = (brightness * period_ns) / data->max_brightness;
    
    /* 设置PWM */
    int ret = pwm_set_pulse_dt(&config->led_pwm, pulse_ns);
    if (ret < 0) {
        LOG_ERR("Failed to set PWM pulse: %d", ret);
        return ret;
    }
    
    data->current_brightness = brightness;
    return 0;
}
#endif

/* GPIO模式：定时器回调 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE && !CONFIG_CHARGING_STATUS_LED_USE_PWM
static void charging_status_led_timer_handler(struct k_timer *timer)
{
    struct charging_status_data *data = CONTAINER_OF(timer, struct charging_status_data, led_timer);
    const struct device *dev = data->dev;
    const struct charging_status_config *config = dev->config;
    
    static bool led_state = false;
    
    if (atomic_get(&data->led_enabled)) {
        /* 切换LED状态 */
        led_state = !led_state;
        gpio_pin_set_dt(&config->led_gpio, led_state ? 1 : 0);
        LOG_DBG("LED %s", led_state ? "ON" : "OFF");
    }
}
#endif

/* 更新LED状态 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
static void update_led_state(const struct device *dev, bool enable)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    
    atomic_set(&data->led_enabled, enable ? 1 : 0);
    
    LOG_DBG("LED state update: %s", enable ? "ENABLE" : "DISABLE");
    
#if CONFIG_CHARGING_STATUS_LED_USE_PWM
    /* PWM模式 */
    if (enable) {
        atomic_set(&data->breathe_active, 1);
        LOG_DBG("Breathe effect activated");
    } else {
        atomic_set(&data->breathe_active, 0);
        /* 关闭LED */
        pwm_set_pulse_dt(&config->led_pwm, 0);
        data->current_brightness = 0;
        LOG_DBG("LED turned off (PWM)");
    }
#else
    /* GPIO模式 */
    if (enable) {
        /* 启动闪烁定时器 */
        k_timer_start(&data->led_timer, K_MSEC(data->blink_interval_ms), K_MSEC(data->blink_interval_ms));
        LOG_DBG("LED blink timer started (%u ms interval)", data->blink_interval_ms);
    } else {
        /* 停止定时器并关闭LED */
        k_timer_stop(&data->led_timer);
        gpio_pin_set_dt(&config->led_gpio, 0);
        LOG_DBG("LED turned off (GPIO)");
    }
#endif
}
#endif

/* LED控制线程 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
static void charging_status_led_thread(void *p1, void *p2, void *p3)
{
    const struct device *dev = (const struct device *)p1;
    struct charging_status_data *data = dev->data;
    
    LOG_INF("Charging status LED thread started for instance %d", 
            data->config->instance);
    
    while (1) {
        /* 只有在充电状态且LED启用时才执行LED效果 */
        if (atomic_get(&data->charging) && atomic_get(&data->led_enabled)) {
#if CONFIG_CHARGING_STATUS_LED_USE_PWM
            if (atomic_get(&data->breathe_active)) {
                /* 计算当前时间在呼吸周期中的位置 */
                uint32_t cycle_time = k_uptime_get_32() % data->breathe_period_ms;
                
                /* 计算当前亮度 */
                uint32_t brightness = calculate_breathe_brightness(
                    cycle_time, 
                    data->breathe_period_ms, 
                    data->max_brightness
                );
                
                /* 设置LED亮度 */
                set_led_brightness_pwm(dev, (uint8_t)brightness);
                
                LOG_DBG("Breathe cycle: %u ms, brightness: %u", 
                        cycle_time, brightness);
                
                /* 等待下一步 */
                k_msleep(data->breathe_step_ms);
                continue;
            }
#endif
        }
        
        /* 非充电状态或LED未启用时，休眠等待 */
        k_msleep(100);
    }
}
#endif

/* LED初始化 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
static void charging_status_led_init(const struct device *dev)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    int ret;
    
    LOG_INF("Initializing LED indicator for instance %d", config->instance);
    
    /* 初始化状态 */
    atomic_set(&data->led_enabled, 0);
    atomic_set(&data->led_active, 0);
    atomic_set(&data->breathe_active, 0);
    
    /* 设置LED参数 */
    data->breathe_period_ms = config->breathe_period_ms;
    data->max_brightness = config->max_brightness;
    data->breathe_step_ms = config->breathe_period_ms / 100;
    data->blink_interval_ms = config->blink_interval_ms;
    data->current_brightness = 0;
    
#if CONFIG_CHARGING_STATUS_LED_USE_PWM
    /* PWM模式初始化 */
    if (!device_is_ready(config->led_pwm.dev)) {
        LOG_ERR("PWM device not ready for LED");
        return;
    }
    
    LOG_INF("PWM LED initialized on %s, channel %d, period %d ns",
            config->led_pwm.dev->name, config->led_pwm.channel, config->led_pwm.period);
    
    /* 初始关闭LED */
    pwm_set_pulse_dt(&config->led_pwm, 0);
#else
    /* GPIO模式初始化 */
    if (!gpio_is_ready_dt(&config->led_gpio)) {
        LOG_ERR("LED GPIO device not ready");
        return;
    }
    
    ret = gpio_pin_configure_dt(&config->led_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED GPIO: %d", ret);
        return;
    }
    
    /* 初始化闪烁定时器 */
    k_timer_init(&data->led_timer, charging_status_led_timer_handler, NULL);
    
    LOG_INF("GPIO LED initialized on pin %s.%d",
            config->led_gpio.port->name, config->led_gpio.pin);
#endif
    
    /* 分配LED线程栈 */
    data->led_thread_stack = k_thread_stack_alloc(CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE, 0);
    if (data->led_thread_stack == NULL) {
        LOG_ERR("Failed to allocate LED thread stack");
        return;
    }
    
    /* 创建LED控制线程 */
    k_thread_create(&data->led_thread,
                   data->led_thread_stack,
                   CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE,
                   charging_status_led_thread,
                   (void *)dev, NULL, NULL,
                   CONFIG_CHARGING_STATUS_THREAD_PRIORITY,
                   0, K_NO_WAIT);
    
    char thread_name[24];
    snprintf(thread_name, sizeof(thread_name), "chg_led_%d", config->instance);
    k_thread_name_set(&data->led_thread, thread_name);
    
    LOG_INF("LED indicator initialized successfully for instance %d", config->instance);
}
#endif

/* 防抖动工作处理器 */
static void charging_status_debounce_work(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct charging_status_data *data = 
        CONTAINER_OF(dwork, struct charging_status_data, debounce_work);
    const struct charging_status_config *config = data->config;
    int current_state;
    int ret;
    
    int64_t start_time = k_uptime_get();
    
    /* 读取稳定的GPIO状态 */
    ret = read_stable_gpio_state(&config->chrg_gpio);
    if (ret < 0) {
        LOG_ERR("Failed to read stable GPIO state: %d", ret);
        return;
    }
    
    current_state = ret;
    
    /* TP4056 CHRG引脚: 充电时低电平，不充电时高电平 */
    int new_charging_status = (current_state == 0);
    int old_charging_status = atomic_get(&data->charging);
    
    LOG_DBG("Debounce work: stable pin state=%d, old_status=%s, new_status=%s",
            current_state,
            old_charging_status ? "CHARGING" : "NOT_CHARGING",
            new_charging_status ? "CHARGING" : "NOT_CHARGING");
    
    /* 检查状态是否稳定足够长时间 */
    int64_t current_time = k_uptime_get();
    if (current_state != data->last_stable_state) {
        /* 状态变化，重置稳定计时 */
        data->last_stable_state = current_state;
        data->stable_since = current_time;
        LOG_DBG("State changed, resetting stable timer");
    } else if ((current_time - data->stable_since) < CONFIG_CHARGING_STATUS_MIN_STABLE_TIME_MS) {
        /* 状态未稳定足够时间，忽略变化 */
        LOG_DBG("State not stable long enough (%lld < %d), ignoring",
                current_time - data->stable_since,
                CONFIG_CHARGING_STATUS_MIN_STABLE_TIME_MS);
        return;
    }
    
    /* 检测可能的硬件故障：频繁状态切换 */
    uint32_t recent_changes = atomic_get(&data->change_count);
    if (recent_changes > 100) {  /* 如果短时间内变化超过100次 */
        atomic_set(&data->hardware_fault, 1);
        LOG_ERR("HARDWARE FAULT DETECTED: Too many state changes (%lu), possible connection issue",
                (unsigned long)recent_changes);
    }
    
    if (new_charging_status != old_charging_status) {
        atomic_set(&data->charging, new_charging_status);
        atomic_inc(&data->change_count);
        data->last_change_time = k_uptime_get();
        
        int64_t processing_time = k_uptime_get() - start_time;
        
        LOG_INF("CHARGING STATUS CHANGED: %s -> %s (processing: %lldms, changes: %ld, interrupts: %ld)",
                old_charging_status ? "CHARGING" : "NOT_CHARGING",
                new_charging_status ? "CHARGING" : "NOT_CHARGING",
                processing_time,
                (long)atomic_get(&data->change_count),
                (long)atomic_get(&data->interrupt_count));
        
        /* 根据充电状态更新LED */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
        if (new_charging_status) {
            update_led_state(data->dev, true);
            LOG_INF("LED enabled (charging)");
        } else {
            update_led_state(data->dev, false);
            LOG_INF("LED disabled (not charging)");
        }
#endif
        
        /* 如果注册了用户回调，则通知 */
        if (data->user_callback) {
            data->user_callback(new_charging_status, data->user_callback_data);
        }
    } else {
        LOG_DBG("Status unchanged: %s (stable pin state: %d)", 
                new_charging_status ? "CHARGING" : "NOT_CHARGING",
                current_state);
    }
}

/* 监控线程函数 */
static void charging_status_monitor_thread(void *p1, void *p2, void *p3)
{
    struct charging_status_data *data = (struct charging_status_data *)p1;
    
    LOG_INF("Charging status monitor thread started for instance %d", 
            data->config->instance);
    
    while (1) {
        int64_t check_time = k_uptime_get();
        
        /* 定期强制检查状态 */
        charging_status_debounce_work(&data->debounce_work.work);
        
        int64_t check_duration = k_uptime_get() - check_time;
        
        LOG_DBG("Periodic check completed in %lldms for instance %d", 
                check_duration, data->config->instance);
        
        /* 检查硬件故障 */
        if (atomic_get(&data->hardware_fault)) {
            LOG_ERR("Hardware fault detected! Check CHRG pin connection");
            /* 可以在这里添加恢复逻辑，比如重置计数器 */
            if (atomic_get(&data->change_count) > 200) {
                atomic_set(&data->change_count, 0);
                atomic_set(&data->interrupt_count, 0);
                LOG_WRN("Reset counters due to hardware fault");
            }
        }
        
        /* 按照配置的间隔休眠 */
        k_msleep(data->config->status_interval_ms);
    }
}

/* 驱动API: 获取当前充电状态 */
static int charging_status_get(const struct device *dev, bool *is_charging)
{
    const struct charging_status_data *data = dev->data;
    
    if (is_charging == NULL) {
        return -EINVAL;
    }
    
    *is_charging = (atomic_get(&data->charging) != 0);
    
    LOG_DBG("Status query: %s", *is_charging ? "CHARGING" : "NOT_CHARGING");
    
    return 0;
}

/* 驱动API: 获取驱动统计信息 */
static int charging_status_get_stats(const struct device *dev, 
                                   int64_t *last_change_time,
                                   uint32_t *change_count,
                                   uint32_t *interrupt_count,
                                   bool *hardware_fault)
{
    const struct charging_status_data *data = dev->data;
    
    if (last_change_time) {
        *last_change_time = data->last_change_time;
    }
    
    if (change_count) {
        *change_count = (uint32_t)atomic_get(&data->change_count);
    }
    
    if (interrupt_count) {
        *interrupt_count = (uint32_t)atomic_get(&data->interrupt_count);
    }
    
    if (hardware_fault) {
        *hardware_fault = (atomic_get(&data->hardware_fault) != 0);
    }
    
    return 0;
}

/* 驱动API: 注册状态变化回调 */
static int charging_status_register_callback(const struct device *dev,
                                           void (*callback)(bool, void*),
                                           void *user_data)
{
    struct charging_status_data *data = dev->data;
    
    if (callback == NULL) {
        return -EINVAL;
    }
    
    data->user_callback = callback;
    data->user_callback_data = user_data;
    
    LOG_DBG("Callback registered for instance %d", data->config->instance);
    
    return 0;
}

/* 驱动API: 获取最后状态变化时间 */
static int charging_status_get_last_change(const struct device *dev,
                                          int64_t *timestamp)
{
    const struct charging_status_data *data = dev->data;
    
    if (timestamp == NULL) {
        return -EINVAL;
    }
    
    *timestamp = data->last_change_time;
    return 0;
}

/* 驱动API: LED控制功能 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
static int charging_status_led_set(const struct device *dev, bool enable)
{
    struct charging_status_data *data = dev->data;
    
    if (!atomic_get(&data->charging)) {
        LOG_WRN("Cannot manually control LED when not charging");
        return -EACCES;
    }
    
    update_led_state(dev, enable);
    return 0;
}

static int charging_status_led_get_state(const struct device *dev, bool *is_active)
{
    struct charging_status_data *data = dev->data;
    
    if (is_active == NULL) {
        return -EINVAL;
    }
    
    *is_active = (atomic_get(&data->led_enabled) != 0);
    return 0;
}

static int charging_status_led_set_params(const struct device *dev,
                                        uint32_t period_ms,
                                        uint8_t max_brightness,
                                        uint32_t blink_interval_ms)
{
    struct charging_status_data *data = dev->data;
    
    if (period_ms < 100 || period_ms > 10000) {
        return -EINVAL;
    }
    
    if (max_brightness > 255) {
        return -EINVAL;
    }
    
    if (blink_interval_ms < 100 || blink_interval_ms > 5000) {
        return -EINVAL;
    }
    
    data->breathe_period_ms = period_ms;
    data->max_brightness = max_brightness;
    data->blink_interval_ms = blink_interval_ms;
    data->breathe_step_ms = period_ms / 100;
    
    LOG_INF("LED params updated: period=%u ms, brightness=%u, blink=%u ms",
            period_ms, max_brightness, blink_interval_ms);
    
    return 0;
}
#endif

/* 驱动API结构 */
struct charging_status_driver_api {
    /* 充电状态相关API */
    int (*get_status)(const struct device *dev, bool *is_charging);
    int (*register_callback)(const struct device *dev,
                           void (*callback)(bool, void*),
                           void *user_data);
    int (*get_last_change)(const struct device *dev, int64_t *timestamp);
    int (*get_stats)(const struct device *dev,
                    int64_t *last_change_time,
                    uint32_t *change_count,
                    uint32_t *interrupt_count,
                    bool *hardware_fault);
    
    /* LED控制API */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
    int (*led_set)(const struct device *dev, bool enable);
    int (*led_get_state)(const struct device *dev, bool *is_active);
    int (*led_set_params)(const struct device *dev,
                         uint32_t period_ms,
                         uint8_t max_brightness,
                         uint32_t blink_interval_ms);
#endif
};

/* 驱动API实例 */
static const struct charging_status_driver_api charging_status_api = {
    .get_status = charging_status_get,
    .register_callback = charging_status_register_callback,
    .get_last_change = charging_status_get_last_change,
    .get_stats = charging_status_get_stats,
#if CONFIG_CHARGING_STATUS_LED_ENABLE
    .led_set = charging_status_led_set,
    .led_get_state = charging_status_led_get_state,
    .led_set_params = charging_status_led_set_params,
#endif
};

/* 驱动初始化 */
static int charging_status_init(const struct device *dev)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    int ret;
    
    /* 存储设备指针 */
    data->dev = dev;
    data->config = config;
    
    /* 先打印简单初始化消息 */
    LOG_INF("Initializing charging status driver instance %d", config->instance);
    
    /* 验证GPIO设备是否就绪 */
    if (!gpio_is_ready_dt(&config->chrg_gpio)) {
        LOG_ERR("CHRG GPIO device not ready for instance %d", config->instance);
        return -ENODEV;
    }
    
    LOG_DBG("GPIO device is ready for instance %d", config->instance);
    
    /* 配置GPIO为输入模式 */
    /* 注意: 硬件已经上拉，所以不需要内部上拉 */
    ret = gpio_pin_configure_dt(&config->chrg_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure CHRG GPIO for instance %d: %d", 
                config->instance, ret);
        return ret;
    }
    
    LOG_DBG("GPIO configured as input for instance %d", config->instance);
    
    /* 初始化GPIO回调 */
    gpio_init_callback(&data->chrg_cb, charging_status_gpio_callback,
                      BIT(config->chrg_gpio.pin));
    
    /* 添加回调以检测上升和下降沿 */
    ret = gpio_add_callback(config->chrg_gpio.port, &data->chrg_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback for instance %d: %d", 
                config->instance, ret);
        return ret;
    }
    
    LOG_DBG("GPIO callback added for instance %d", config->instance);
    
    /* 配置中断为双边沿触发 */
    ret = gpio_pin_interrupt_configure_dt(&config->chrg_gpio,
                                         GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO interrupt for instance %d: %d", 
                config->instance, ret);
        return ret;
    }
    
    LOG_INF("GPIO interrupt configured for pin %s.%d (instance %d)",
            config->chrg_gpio.port->name, config->chrg_gpio.pin,
            config->instance);
    
    /* 初始化防抖动工作 */
    k_work_init_delayable(&data->debounce_work, charging_status_debounce_work);
    
    /* 初始状态读取 - 使用稳定读取 */
    int initial_state = read_stable_gpio_state(&config->chrg_gpio);
    if (initial_state < 0) {
        LOG_ERR("Failed to read initial GPIO state for instance %d: %d", 
                config->instance, initial_state);
        initial_state = 1; /* 默认假设不充电 */
    }
    
    /* 设置初始状态 */
    atomic_set(&data->charging, (initial_state == 0));
    data->last_stable_state = initial_state;
    data->stable_since = k_uptime_get();
    data->last_change_time = k_uptime_get();
    
    LOG_INF("INITIAL STATUS: %s (GPIO pin %s.%d state: %d, instance: %d)",
            (initial_state == 0) ? "CHARGING" : "NOT_CHARGING",
            config->chrg_gpio.port->name, config->chrg_gpio.pin,
            initial_state, config->instance);
    
    /* 分配线程栈 */
    data->thread_stack = k_thread_stack_alloc(CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE, 0);
    if (data->thread_stack == NULL) {
        LOG_ERR("Failed to allocate thread stack for instance %d", config->instance);
        return -ENOMEM;
    }
    
    LOG_DBG("Thread stack allocated for instance %d", config->instance);
    
    /* 创建监控线程 */
    k_thread_create(&data->monitor_thread,
                   data->thread_stack,
                   CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE,
                   charging_status_monitor_thread,
                   data, NULL, NULL,
                   CONFIG_CHARGING_STATUS_THREAD_PRIORITY,
                   0, K_NO_WAIT);
    
    char thread_name[24];
    snprintf(thread_name, sizeof(thread_name), "chg_mon_%d", config->instance);
    k_thread_name_set(&data->monitor_thread, thread_name);
    
    LOG_INF("Charging status driver initialized successfully for instance %d", 
            config->instance);
    
    /* 初始化LED指示功能 */
#if CONFIG_CHARGING_STATUS_LED_ENABLE
    charging_status_led_init(dev);
#endif
    
    /* 立即执行一次初始状态检查 */
    k_work_schedule(&data->debounce_work, K_MSEC(10));
    
    return 0;
}

/* 设备树实例化宏 */
#define CHARGING_STATUS_INIT(n)                                               \
    static struct charging_status_data charging_status_data_##n = {           \
        .charging = ATOMIC_INIT(0),                                           \
        .change_count = ATOMIC_INIT(0),                                       \
        .interrupt_count = ATOMIC_INIT(0),                                    \
        .hardware_fault = ATOMIC_INIT(0),                                     \
        .last_change_time = 0,                                                \
        .last_interrupt_time = 0,                                             \
        .last_stable_state = -1,                                              \
        .stable_since = 0,                                                    \
        .user_callback = NULL,                                                \
        .user_callback_data = NULL,                                           \
    };                                                                        \
                                                                              \
    static const struct charging_status_config charging_status_config_##n = { \
        .chrg_gpio = GPIO_DT_SPEC_INST_GET(n, chrg_gpios),                   \
        .status_interval_ms = DT_INST_PROP_OR(n, status_interval_ms, 1000),  \
        .instance = n,                                                        \
        COND_CODE_1(CONFIG_CHARGING_STATUS_LED_ENABLE,                        \
            (                                                                 \
                COND_CODE_1(CONFIG_CHARGING_STATUS_LED_USE_PWM,               \
                    (.led_pwm = PWM_DT_SPEC_INST_GET_BY_IDX(n, 0),),          \
                    (.led_gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_DRV_INST(n), led_gpios, 0),) \
                ),                                                            \
                .breathe_period_ms = DT_INST_PROP_OR(n, breathe_period_ms,    \
                    CONFIG_CHARGING_STATUS_LED_BREATHE_PERIOD_MS),            \
                .blink_interval_ms = DT_INST_PROP_OR(n, blink_interval_ms, 500), \
                .max_brightness = DT_INST_PROP_OR(n, max_brightness,          \
                    CONFIG_CHARGING_STATUS_LED_MAX_BRIGHTNESS),               \
            ),                                                                \
            ()                                                                \
        )                                                                     \
    };                                                                        \
                                                                              \
    DEVICE_DT_INST_DEFINE(n,                                                  \
                         charging_status_init,                                \
                         NULL,                                                \
                         &charging_status_data_##n,                           \
                         &charging_status_config_##n,                         \
                         POST_KERNEL,                                         \
                         CONFIG_CHARGING_STATUS_INIT_PRIORITY,                \
                         &charging_status_api);

/* 从设备树实例化所有实例 */
DT_INST_FOREACH_STATUS_OKAY(CHARGING_STATUS_INIT)