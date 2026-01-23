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
#define CONFIG_CHARGING_STATUS_LOG_LEVEL 3
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
};

/* 驱动配置结构 */
struct charging_status_config {
    /* CHRG GPIO规格 */
    struct gpio_dt_spec chrg_gpio;
    
    /* 轮询间隔 (毫秒) */
    uint32_t status_interval_ms;
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
    
    /* 获取当前GPIO状态 */
    if (gpio_pin_get_dt(&config->chrg_gpio) < 0) {
        LOG_ERR("Failed to get GPIO state");
        return;
    }
    
    current_state = gpio_pin_get_dt(&config->chrg_gpio);
    
    /* TP4056 CHRG引脚: 充电时低电平，不充电时高电平 */
    /* 注意: GPIO_ACTIVE_LOW意味着逻辑低电平表示活动状态 */
    int new_charging_status = (current_state == 0);
    int old_charging_status = atomic_get(&data->charging);
    
    if (new_charging_status != old_charging_status) {
        atomic_set(&data->charging, new_charging_status);
        data->last_change_time = k_uptime_get();
        
        LOG_DBG("Charging status changed from %s to %s",
                old_charging_status ? "CHARGING" : "NOT_CHARGING",
                new_charging_status ? "CHARGING" : "NOT_CHARGING");
        
        /* 如果注册了用户回调，则通知 */
        if (data->user_callback) {
            data->user_callback(new_charging_status, data->user_callback_data);
        }
    }
}

/* 监控线程函数 */
static void charging_status_monitor_thread(void *p1, void *p2, void *p3)
{
    struct charging_status_data *data = (struct charging_status_data *)p1;
    
    LOG_DBG("Charging status monitor thread started");
    
    while (1) {
        /* 定期强制检查状态 */
        charging_status_debounce_work(&data->debounce_work.work);
        
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
};

/* 驱动API实例 */
static const struct charging_status_driver_api charging_status_api = {
    .get_status = charging_status_get,
    .register_callback = charging_status_register_callback,
    .get_last_change = charging_status_get_last_change,
};

/* 驱动初始化 */
static int charging_status_init(const struct device *dev)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    int ret;
    
    /* 存储配置引用 */
    data->config = config;
    
    /* 验证GPIO设备是否就绪 */
    if (!gpio_is_ready_dt(&config->chrg_gpio)) {
        LOG_ERR("CHRG GPIO device not ready");
        return -ENODEV;
    }
    
    /* 配置GPIO为输入模式 */
    /* 注意: 硬件已经上拉，所以不需要内部上拉 */
    ret = gpio_pin_configure_dt(&config->chrg_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure CHRG GPIO: %d", ret);
        return ret;
    }
    
    /* 初始化GPIO回调 */
    gpio_init_callback(&data->chrg_cb, charging_status_gpio_callback,
                      BIT(config->chrg_gpio.pin));
    
    /* 添加回调以检测上升和下降沿 */
    ret = gpio_add_callback(config->chrg_gpio.port, &data->chrg_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback: %d", ret);
        return ret;
    }
    
    /* 配置中断为双边沿触发 */
    ret = gpio_pin_interrupt_configure_dt(&config->chrg_gpio,
                                         GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO interrupt: %d", ret);
        return ret;
    }
    
    /* 初始化防抖动工作 */
    k_work_init_delayable(&data->debounce_work, charging_status_debounce_work);
    
    /* 初始状态读取 */
    int initial_state = gpio_pin_get_dt(&config->chrg_gpio);
    if (initial_state < 0) {
        LOG_ERR("Failed to read initial GPIO state: %d", initial_state);
        initial_state = 1; /* 默认假设不充电 */
    }
    
    /* 设置初始状态 */
    atomic_set(&data->charging, (initial_state == 0));
    data->last_change_time = k_uptime_get();
    
    LOG_INF("Charging status initialized on pin %s.%d: %s (state: %d)",
            config->chrg_gpio.port->name, config->chrg_gpio.pin,
            (initial_state == 0) ? "CHARGING" : "NOT_CHARGING", initial_state);
    
    /* 分配线程栈 */
    data->thread_stack = k_thread_stack_alloc(CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE, 0);
    if (data->thread_stack == NULL) {
        LOG_ERR("Failed to allocate thread stack");
        return -ENOMEM;
    }
    
    /* 创建监控线程 */
    k_thread_create(&data->monitor_thread,
                   data->thread_stack,
                   CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE,
                   charging_status_monitor_thread,
                   data, NULL, NULL,
                   CONFIG_CHARGING_STATUS_THREAD_PRIORITY,
                   0, K_NO_WAIT);
    
    k_thread_name_set(&data->monitor_thread, "charging_monitor");
    
    LOG_DBG("Charging status driver initialized successfully");
    return 0;
}

/* 设备树实例化宏 */
#define CHARGING_STATUS_INIT(n)                                               \
    static struct charging_status_data charging_status_data_##n = {           \
        .charging = ATOMIC_INIT(0),                                           \
        .last_change_time = 0,                                                \
        .user_callback = NULL,                                                \
        .user_callback_data = NULL,                                           \
    };                                                                        \
                                                                              \
    static const struct charging_status_config charging_status_config_##n = { \
        .chrg_gpio = GPIO_DT_SPEC_INST_GET(n, chrg_gpios),                   \
        .status_interval_ms = DT_INST_PROP_OR(n, status_interval_ms, 1000),  \
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