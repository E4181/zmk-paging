/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * Charging Status Driver for TP4056
 * Uses interrupt mode for real-time charging status detection
 */

#define DT_DRV_COMPAT zmk_charging_status

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>

/* 直接在驱动文件中定义Kconfig宏 */
#ifndef CONFIG_CHARGING_STATUS_LOG_LEVEL
#define CONFIG_CHARGING_STATUS_LOG_LEVEL 4  /* DEBUG级别 */
#endif

#ifndef CONFIG_CHARGING_STATUS_INIT_PRIORITY
#define CONFIG_CHARGING_STATUS_INIT_PRIORITY 90
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
};

/* 驱动配置结构 */
struct charging_status_config {
    /* CHRG GPIO规格 */
    struct gpio_dt_spec chrg_gpio;
    
    /* 轮询间隔 (毫秒) */
    uint32_t status_interval_ms;
    
    /* 设备实例编号 */
    uint8_t instance;
};

/* 前向声明 */
static void charging_status_debounce_work(struct k_work *work);
static void charging_status_monitor_thread(void *p1, void *p2, void *p3);

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

/* 驱动API结构 */
struct charging_status_driver_api {
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
};

/* 驱动API实例 */
static const struct charging_status_driver_api charging_status_api = {
    .get_status = charging_status_get,
    .register_callback = charging_status_register_callback,
    .get_last_change = charging_status_get_last_change,
    .get_stats = charging_status_get_stats,
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