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

/* 直接在驱动文件中定义Kconfig宏 */
#ifndef CONFIG_CHARGING_STATUS_LOG_LEVEL
#define CONFIG_CHARGING_STATUS_LOG_LEVEL 4  /* 默认: DEBUG级别以便调试 */
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

/* 日志模块注册 */
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
    
    /* 立即调度防抖动工作 - 不延迟，因为我们已经在中断上下文中 */
    k_work_submit(&data->debounce_work.work);
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
    
    /* 获取当前GPIO状态 */
    ret = gpio_pin_get_dt(&config->chrg_gpio);
    if (ret < 0) {
        LOG_ERR("Failed to get GPIO state: %d", ret);
        return;
    }
    
    current_state = ret;
    
    /* TP4056 CHRG引脚: 充电时低电平，不充电时高电平 */
    /* 注意: GPIO_ACTIVE_LOW意味着逻辑低电平表示活动状态 */
    int new_charging_status = (current_state == 0);
    int old_charging_status = atomic_get(&data->charging);
    
    LOG_DBG("Debounce work: pin state=%d, old_status=%s, new_status=%s",
            current_state,
            old_charging_status ? "CHARGING" : "NOT_CHARGING",
            new_charging_status ? "CHARGING" : "NOT_CHARGING");
    
    if (new_charging_status != old_charging_status) {
        atomic_set(&data->charging, new_charging_status);
        atomic_inc(&data->change_count);
        data->last_change_time = k_uptime_get();
        
        int64_t processing_time = k_uptime_get() - start_time;
        
        /* 记录状态变化信息 */
        LOG_INF("CHARGING STATUS CHANGED: %s -> %s (processing: %lldms, changes: %d, interrupts: %d)",
                old_charging_status ? "CHARGING" : "NOT_CHARGING",
                new_charging_status ? "CHARGING" : "NOT_CHARGING",
                processing_time,
                atomic_get(&data->change_count),
                atomic_get(&data->interrupt_count));
        
        /* 如果注册了用户回调，则通知 */
        if (data->user_callback) {
            data->user_callback(new_charging_status, data->user_callback_data);
        }
    } else {
        LOG_DBG("Status unchanged: %s (pin state: %d)", 
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
        
        /* 记录周期性检查信息 */
        LOG_DBG("Periodic check completed in %lldms for instance %d", 
                check_duration, data->config->instance);
        
        /* 按照配置的间隔休眠 */
        k_msleep(data->config->status_interval_ms);
    }
}

/* 驱动API: 获取当前充电状态 */
static int charging_status_get(const struct device *dev, bool *is_charging)
{
    const struct charging_status_data *data = dev->data;
    int64_t start_time = k_uptime_get();
    
    if (is_charging == NULL) {
        return -EINVAL;
    }
    
    *is_charging = (atomic_get(&data->charging) != 0);
    
    LOG_DBG("Status query: %s (query time: %lldms)",
            *is_charging ? "CHARGING" : "NOT_CHARGING",
            k_uptime_get() - start_time);
    
    return 0;
}

/* 驱动API: 获取驱动统计信息 */
static int charging_status_get_stats(const struct device *dev, 
                                   int64_t *last_change_time,
                                   uint32_t *change_count,
                                   uint32_t *interrupt_count)
{
    const struct charging_status_data *data = dev->data;
    
    if (last_change_time) {
        *last_change_time = data->last_change_time;
    }
    
    if (change_count) {
        *change_count = atomic_get(&data->change_count);
    }
    
    if (interrupt_count) {
        *interrupt_count = atomic_get(&data->interrupt_count);
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
                    uint32_t *interrupt_count);
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
    
    LOG_INF("Initializing charging status driver for instance %d on pin %s.%d",
            config->instance,
            config->chrg_gpio.port ? config->chrg_gpio.port->name : "NULL",
            config->chrg_gpio.pin);
    
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
    
    LOG_INF("GPIO interrupt configured for bilateral edge detection on instance %d",
            config->instance);
    
    /* 初始化防抖动工作 */
    k_work_init_delayable(&data->debounce_work, charging_status_debounce_work);
    
    /* 初始状态读取 */
    int initial_state = gpio_pin_get_dt(&config->chrg_gpio);
    if (initial_state < 0) {
        LOG_ERR("Failed to read initial GPIO state for instance %d: %d", 
                config->instance, initial_state);
        initial_state = 1; /* 默认假设不充电 */
    }
    
    /* 设置初始状态 */
    atomic_set(&data->charging, (initial_state == 0));
    data->last_change_time = k_uptime_get();
    
    LOG_INF("INITIAL CHARGING STATUS for instance %d: %s (GPIO pin %s.%d state: %d)",
            config->instance,
            (initial_state == 0) ? "CHARGING" : "NOT_CHARGING",
            config->chrg_gpio.port->name, config->chrg_gpio.pin,
            initial_state);
    
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
    
    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "charging_monitor_%d", config->instance);
    k_thread_name_set(&data->monitor_thread, thread_name);
    
    LOG_INF("Charging status driver for instance %d initialized successfully", 
            config->instance);
    
    /* 初始状态检查 */
    charging_status_debounce_work(&data->debounce_work.work);
    
    return 0;
}

/* 设备树实例化宏 */
#define CHARGING_STATUS_INIT(n)                                               \
    static struct charging_status_data charging_status_data_##n = {           \
        .charging = ATOMIC_INIT(0),                                           \
        .change_count = ATOMIC_INIT(0),                                       \
        .interrupt_count = ATOMIC_INIT(0),                                    \
        .last_change_time = 0,                                                \
        .last_interrupt_time = 0,                                             \
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