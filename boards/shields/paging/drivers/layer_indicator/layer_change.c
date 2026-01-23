/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_layer_change

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include "layer_indicator.h"

LOG_MODULE_REGISTER(layer_change, CONFIG_ZMK_LOG_LEVEL);

/* 配置结构体 */
struct layer_change_config {
    struct gpio_dt_spec led;          /* LED GPIO配置 */
    uint8_t target_layer;             /* 目标层数 */
    uint16_t blink_on_ms;             /* LED亮的时间 */
    uint16_t blink_off_ms;            /* LED灭的时间 */
    uint16_t blink_count;             /* 闪烁次数 (0=持续) */
    bool invert_led;                  /* LED极性反转 */
};

/* 状态结构体 */
struct layer_change_data {
    const struct device *dev;
    struct k_work_delayable blink_work;  /* 闪烁工作队列 */
    struct k_work_delayable stop_work;   /* 停止闪烁工作队列 */
    const struct device *layer_indicator; /* 层指示器设备 */
    bool enabled;                        /* 功能是否启用 */
    bool blinking;                       /* 是否正在闪烁 */
    uint8_t current_layer_count;         /* 当前层数 */
    uint8_t blink_counter;               /* 闪烁计数器 */
    bool led_state;                      /* LED当前状态 */
};

/* LED控制函数 */
static void set_led_state(const struct device *dev, bool state) {
    struct layer_change_data *data = dev->data;
    const struct layer_change_config *cfg = dev->config;
    
    if (!gpio_is_ready_dt(&cfg->led)) {
        LOG_WRN("LED GPIO not ready");
        return;
    }
    
    /* 根据极性设置LED */
    int value = cfg->invert_led ? !state : state;
    gpio_pin_set_dt(&cfg->led, value);
    data->led_state = state;
}

/* 闪烁工作回调 */
static void blink_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct layer_change_data *data = 
        CONTAINER_OF(dwork, struct layer_change_data, blink_work);
    const struct device *dev = data->dev;
    const struct layer_change_config *cfg = dev->config;
    
    if (!data->enabled || !data->blinking) {
        return;
    }
    
    /* 翻转LED状态 */
    bool new_state = !data->led_state;
    set_led_state(dev, new_state);
    
    /* 设置下一次闪烁 */
    uint32_t delay = new_state ? cfg->blink_on_ms : cfg->blink_off_ms;
    
    /* 如果不是持续闪烁，检查计数 */
    if (cfg->blink_count > 0) {
        if (!new_state) {  /* 每次完整闪烁（亮+灭）计数一次 */
            data->blink_counter++;
            if (data->blink_counter >= cfg->blink_count) {
                data->blinking = false;
                set_led_state(dev, false); /* 最终关闭LED */
                LOG_DBG("Blink completed, counter: %d", data->blink_counter);
                return;
            }
        }
    }
    
    /* 调度下一次闪烁 */
    k_work_reschedule(&data->blink_work, K_MSEC(delay));
}

/* 停止闪烁工作回调 */
static void stop_blink_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct layer_change_data *data = 
        CONTAINER_OF(dwork, struct layer_change_data, stop_work);
    
    data->blinking = false;
    k_work_cancel_delayable(&data->blink_work);
    set_led_state(data->dev, false);
    LOG_DBG("Blink stopped");
}

/* 开始闪烁 */
static void start_blinking(const struct device *dev) {
    struct layer_change_data *data = dev->data;
    const struct layer_change_config *cfg = dev->config;
    
    if (!data->enabled || data->blinking) {
        return;
    }
    
    data->blinking = true;
    data->blink_counter = 0;
    
    /* 立即打开LED */
    set_led_state(dev, true);
    
    /* 如果不是持续闪烁，设置停止定时器 */
    if (cfg->blink_count > 0) {
        uint32_t total_time = cfg->blink_count * (cfg->blink_on_ms + cfg->blink_off_ms);
        k_work_reschedule(&data->stop_work, K_MSEC(total_time + 100)); /* 加100ms缓冲 */
    }
    
    /* 开始闪烁循环 */
    k_work_reschedule(&data->blink_work, K_MSEC(cfg->blink_on_ms));
    
    LOG_INF("Started blinking LED for layer %d", cfg->target_layer);
}

/* 检查层变化并触发LED */
static void check_layer_and_blink(const struct device *dev) {
    struct layer_change_data *data = dev->data;
    const struct layer_change_config *cfg = dev->config;
    
    if (!data->enabled || !data->layer_indicator) {
        return;
    }
    
    /* 获取当前层数 */
    uint8_t layer_count;
    int ret = layer_indicator_get_count(data->layer_indicator, &layer_count);
    
    if (ret != 0) {
        LOG_ERR("Failed to get layer count: %d", ret);
        return;
    }
    
    /* 记录层数变化 */
    if (layer_count != data->current_layer_count) {
        LOG_DBG("Layer count changed: %d -> %d", 
                data->current_layer_count, layer_count);
        data->current_layer_count = layer_count;
    }
    
    /* 检查是否达到目标层 */
    if (layer_count == cfg->target_layer) {
        if (!data->blinking) {
            LOG_INF("Target layer %d reached, activating LED", cfg->target_layer);
            start_blinking(dev);
        }
    } else {
        if (data->blinking) {
            LOG_INF("Left target layer %d, stopping LED", cfg->target_layer);
            data->blinking = false;
            k_work_cancel_delayable(&data->blink_work);
            k_work_cancel_delayable(&data->stop_work);
            set_led_state(dev, false);
        }
    }
}

/* 层状态变化事件处理 */
static int handle_layer_state_changed(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    /* 遍历所有layer_change设备实例 */
    const struct device *dev;
    struct layer_change_data *data;
    
    for (int i = 0; (dev = DEVICE_DT_INST_GET(i)); i++) {
        if (!device_is_ready(dev)) {
            continue;
        }
        
        data = dev->data;
        if (data->enabled) {
            check_layer_and_blink(dev);
        }
    }
    
    return ZMK_EV_EVENT_BUBBLE;
}

/* API: 启用功能 */
static int layer_change_enable_api(const struct device *dev) {
    struct layer_change_data *data = dev->data;
    
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    
    if (data->enabled) {
        return 0; /* 已经启用 */
    }
    
    data->enabled = true;
    
    /* 立即检查当前层状态 */
    check_layer_and_blink(dev);
    
    LOG_INF("Layer change LED enabled for target layer %d", 
           ((struct layer_change_config *)dev->config)->target_layer);
    
    return 0;
}

/* API: 禁用功能 */
static int layer_change_disable_api(const struct device *dev) {
    struct layer_change_data *data = dev->data;
    
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    
    if (!data->enabled) {
        return 0; /* 已经禁用 */
    }
    
    data->enabled = false;
    data->blinking = false;
    
    /* 停止所有工作队列 */
    k_work_cancel_delayable(&data->blink_work);
    k_work_cancel_delayable(&data->stop_work);
    
    /* 关闭LED */
    set_led_state(dev, false);
    
    LOG_INF("Layer change LED disabled");
    
    return 0;
}

/* API: 检查功能是否启用 */
static bool layer_change_is_enabled_api(const struct device *dev) {
    if (!device_is_ready(dev)) {
        return false;
    }
    
    struct layer_change_data *data = dev->data;
    return data->enabled;
}

/* 驱动初始化 */
static int layer_change_init(const struct device *dev) {
    struct layer_change_data *data = dev->data;
    const struct layer_change_config *cfg = dev->config;
    
    data->dev = dev;
    data->enabled = true; /* 默认启用 */
    data->blinking = false;
    data->blink_counter = 0;
    data->led_state = false;
    
    /* 初始化工作队列 */
    k_work_init_delayable(&data->blink_work, blink_work_cb);
    k_work_init_delayable(&data->stop_work, stop_blink_work_cb);
    
    /* 获取layer_indicator设备 */
    data->layer_indicator = DEVICE_DT_GET(DT_NODELABEL(layer_indicator));
    if (!device_is_ready(data->layer_indicator)) {
        LOG_WRN("Layer indicator device not ready, deferring initialization");
    }
    
    /* 配置LED GPIO */
    if (gpio_is_ready_dt(&cfg->led)) {
        int ret = gpio_pin_configure_dt(&cfg->led, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure LED GPIO: %d", ret);
        } else {
            LOG_DBG("LED GPIO configured on pin %d", cfg->led.pin);
        }
    } else {
        LOG_WRN("LED GPIO not ready at initialization");
    }
    
    /* 记录配置 */
    LOG_INF("Layer change initialized: target=%d, blink=%d/%dms, count=%d",
           cfg->target_layer, cfg->blink_on_ms, cfg->blink_off_ms, cfg->blink_count);
    
    return 0;
}

/* 驱动API结构体 */
struct layer_change_api {
    int (*enable)(const struct device *dev);
    int (*disable)(const struct device *dev);
    bool (*is_enabled)(const struct device *dev);
};

static const struct layer_change_api layer_change_api_funcs = {
    .enable = layer_change_enable_api,
    .disable = layer_change_disable_api,
    .is_enabled = layer_change_is_enabled_api,
};

/* 事件监听器注册 */
ZMK_LISTENER(layer_change, handle_layer_state_changed);
ZMK_SUBSCRIPTION(layer_change, zmk_layer_state_changed);

/* 设备实例化 */
#define LAYER_CHANGE_INIT(n) \
    static struct layer_change_data layer_change_data_##n; \
    static const struct layer_change_config layer_change_config_##n = { \
        .led = GPIO_DT_SPEC_INST_GET(n, led_gpios), \
        .target_layer = DT_INST_PROP(n, target_layer), \
        .blink_on_ms = DT_INST_PROP_OR(n, blink_on_ms, 100), \
        .blink_off_ms = DT_INST_PROP_OR(n, blink_off_ms, 100), \
        .blink_count = DT_INST_PROP_OR(n, blink_count, 0), \
        .invert_led = DT_INST_PROP_OR(n, invert_led, false), \
    }; \
    DEVICE_DT_INST_DEFINE(n, layer_change_init, NULL, \
                         &layer_change_data_##n, &layer_change_config_##n, \
                         POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                         &layer_change_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(LAYER_CHANGE_INIT)

/* 导出API函数 */
int layer_change_enable(const struct device *dev) {
    const struct layer_change_api *api = dev->api;
    return api->enable(dev);
}

int layer_change_disable(const struct device *dev) {
    const struct layer_change_api *api = dev->api;
    return api->disable(dev);
}

bool layer_change_is_enabled(const struct device *dev) {
    const struct layer_change_api *api = dev->api;
    return api->is_enabled(dev);
}