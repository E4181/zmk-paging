/*
 * Copyright (c) 2024
 * Charging Status Driver for TP4056
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(charging_status, CONFIG_CHARGING_STATUS_LOG_LEVEL);

/* 直接在驱动文件中定义Kconfig宏 */
#ifndef CONFIG_CHARGING_STATUS_ENABLE
#define CONFIG_CHARGING_STATUS_ENABLE 1
#endif

#ifndef CONFIG_CHARGING_STATUS_LOG_LEVEL
#define CONFIG_CHARGING_STATUS_LOG_LEVEL 2
#endif

/* 充电状态枚举 */
enum charging_status_state {
    CHARGING_STATUS_NOT_CHARGING = 0,
    CHARGING_STATUS_CHARGING,
    CHARGING_STATUS_UNKNOWN
};

/* 设备私有数据结构 */
struct charging_status_data {
    /* GPIO回调结构 */
    struct gpio_callback gpio_cb;
    
    /* 状态变化回调函数 */
    void (*status_changed_cb)(const struct device *dev, enum charging_status_state state);
    
    /* 防抖工作队列 */
    struct k_work_delayable debounce_work;
    
    /* 当前状态 */
    enum charging_status_state current_state;
    
    /* 中断是否已启用 */
    bool interrupt_enabled;
};

/* 设备配置数据结构 */
struct charging_status_config {
    /* GPIO规格 */
    struct gpio_dt_spec chrg_gpio;
    
    /* 是否低电平有效 */
    uint8_t active_low;
    
    /* 防抖时间（毫秒） */
    uint32_t debounce_ms;
};

/* 防抖工作处理函数 */
static void debounce_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct charging_status_data *data = CONTAINER_OF(dwork, struct charging_status_data, debounce_work);
    const struct device *dev = CONTAINER_OF(data, const struct device, data);
    const struct charging_status_config *config = dev->config;
    
    int pin_state = gpio_pin_get_dt(&config->chrg_gpio);
    
    if (pin_state < 0) {
        LOG_ERR("Failed to read CHRG pin");
        return;
    }
    
    /* 根据active_low配置转换状态 */
    bool is_charging;
    if (config->active_low == 1) {
        is_charging = (pin_state == 0);
    } else {
        is_charging = (pin_state == 1);
    }
    
    enum charging_status_state new_state = is_charging ? 
        CHARGING_STATUS_CHARGING : CHARGING_STATUS_NOT_CHARGING;
    
    /* 状态变化时调用回调 */
    if (new_state != data->current_state) {
        data->current_state = new_state;
        
        LOG_INF("Charging status changed: %s", 
                new_state == CHARGING_STATUS_CHARGING ? "CHARGING" : "NOT CHARGING");
        
        if (data->status_changed_cb != NULL) {
            data->status_changed_cb(dev, new_state);
        }
    }
    
    /* 重新启用中断 */
    if (data->interrupt_enabled) {
        gpio_pin_interrupt_configure_dt(&config->chrg_gpio, GPIO_INT_EDGE_BOTH);
    }
}

/* GPIO中断回调函数 */
static void chrg_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    struct charging_status_data *data = CONTAINER_OF(cb, struct charging_status_data, gpio_cb);
    const struct charging_status_config *config = dev->config;
    
    /* 禁用中断以防抖动 */
    gpio_pin_interrupt_configure_dt(&config->chrg_gpio, GPIO_INT_DISABLE);
    
    /* 调度防抖工作 */
    k_work_reschedule(&data->debounce_work, K_MSEC(config->debounce_ms));
}

/* 获取当前充电状态 */
enum charging_status_state charging_status_get_state(const struct device *dev)
{
    struct charging_status_data *data = dev->data;
    
    return data->current_state;
}

/* 设置状态变化回调函数 */
int charging_status_set_callback(const struct device *dev, 
                                void (*callback)(const struct device *dev, 
                                                enum charging_status_state state))
{
    struct charging_status_data *data = dev->data;
    
    data->status_changed_cb = callback;
    return 0;
}

/* 启用/禁用中断 */
int charging_status_set_interrupt(const struct device *dev, bool enable)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    int ret = 0;
    
    if (enable && !data->interrupt_enabled) {
        ret = gpio_pin_interrupt_configure_dt(&config->chrg_gpio, GPIO_INT_EDGE_BOTH);
        if (ret == 0) {
            data->interrupt_enabled = true;
        }
    } else if (!enable && data->interrupt_enabled) {
        ret = gpio_pin_interrupt_configure_dt(&config->chrg_gpio, GPIO_INT_DISABLE);
        data->interrupt_enabled = false;
    }
    
    return ret;
}

/* 手动触发一次状态读取（用于轮询模式） */
int charging_status_update(const struct device *dev)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    
    int pin_state = gpio_pin_get_dt(&config->chrg_gpio);
    if (pin_state < 0) {
        return pin_state;
    }
    
    /* 根据active_low配置转换状态 */
    bool is_charging;
    if (config->active_low == 1) {
        is_charging = (pin_state == 0);
    } else {
        is_charging = (pin_state == 1);
    }
    
    enum charging_status_state new_state = is_charging ? 
        CHARGING_STATUS_CHARGING : CHARGING_STATUS_NOT_CHARGING;
    
    /* 状态变化时调用回调 */
    if (new_state != data->current_state) {
        data->current_state = new_state;
        
        LOG_INF("Charging status changed: %s", 
                new_state == CHARGING_STATUS_CHARGING ? "CHARGING" : "NOT CHARGING");
        
        if (data->status_changed_cb != NULL) {
            data->status_changed_cb(dev, new_state);
        }
    }
    
    return 0;
}

/* 设备初始化 */
static int charging_status_init(const struct device *dev)
{
    const struct charging_status_config *config = dev->config;
    struct charging_status_data *data = dev->data;
    int ret;
    
    /* 检查GPIO设备是否就绪 */
    if (!device_is_ready(config->chrg_gpio.port)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }
    
    /* 配置GPIO为输入模式 */
    ret = gpio_pin_configure_dt(&config->chrg_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure CHRG GPIO");
        return ret;
    }
    
    /* 初始化GPIO回调 */
    gpio_init_callback(&data->gpio_cb, chrg_gpio_callback, BIT(config->chrg_gpio.pin));
    
    /* 添加回调 */
    ret = gpio_add_callback(config->chrg_gpio.port, &data->gpio_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback");
        return ret;
    }
    
    /* 初始化防抖工作队列 */
    k_work_init_delayable(&data->debounce_work, debounce_work_handler);
    
    /* 初始化状态 */
    data->current_state = CHARGING_STATUS_UNKNOWN;
    data->status_changed_cb = NULL;
    data->interrupt_enabled = false;
    
    /* 读取初始状态 */
    ret = charging_status_update(dev);
    if (ret < 0) {
        LOG_ERR("Failed to read initial charging status");
        return ret;
    }
    
    /* 启用中断 */
    ret = charging_status_set_interrupt(dev, true);
    if (ret < 0) {
        LOG_ERR("Failed to enable interrupt");
        return ret;
    }
    
    LOG_INF("Charging status driver initialized");
    return 0;
}

/* 设备API结构体 */
static const struct charging_status_driver_api charging_status_api = {
    .get_state = charging_status_get_state,
    .set_callback = charging_status_set_callback,
    .set_interrupt = charging_status_set_interrupt,
    .update = charging_status_update,
};

/* 从设备树实例化设备 */
#define CHARGING_STATUS_DEVICE_INIT(inst)                                     \
    static struct charging_status_data charging_status_data_##inst = {        \
        .gpio_cb = GPIO_CALLBACK_INIT,                                       \
    };                                                                       \
                                                                             \
    static const struct charging_status_config charging_status_config_##inst = { \
        .chrg_gpio = GPIO_DT_SPEC_INST_GET(inst, chrg_gpios),               \
        .active_low = DT_INST_PROP(inst, status_active_low),                 \
        .debounce_ms = DT_INST_PROP_OR(inst, debounce_ms, 50),               \
    };                                                                       \
                                                                             \
    DEVICE_DT_INST_DEFINE(inst,                                              \
                         charging_status_init,                               \
                         NULL,                                               \
                         &charging_status_data_##inst,                       \
                         &charging_status_config_##inst,                     \
                         POST_KERNEL,                                        \
                         CONFIG_APPLICATION_INIT_PRIORITY,                   \
                         &charging_status_api);

/* 自动初始化所有匹配的设备树实例 */
DT_INST_FOREACH_STATUS_OKAY(CHARGING_STATUS_DEVICE_INIT)