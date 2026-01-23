/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_layer_indicator

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>

LOG_MODULE_REGISTER(layer_indicator, CONFIG_ZMK_LOG_LEVEL);

/* 驱动配置 */
struct layer_indicator_config {
    bool log_all_transitions;
};

/* 驱动状态 */
struct layer_indicator_data {
    struct k_mutex lock;
    uint8_t active_layers;      /* 当前激活的层数 */
    uint8_t highest_layer;      /* 最高激活层 */
    int64_t last_change;        /* 最后变化时间 */
    bool layer_state[ZMK_KEYMAP_LAYERS_LEN]; /* 每层状态 */
};

/* 更新层状态并计算统计数据 */
static void update_layer_stats(struct layer_indicator_data *data) {
    uint8_t count = 0;
    uint8_t highest = 0;
    
    for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        if (data->layer_state[i]) {
            count++;
            highest = i;
        }
    }
    
    data->active_layers = count;
    data->highest_layer = highest;
    data->last_change = k_uptime_get();
}

/* 事件处理函数 */
static int handle_layer_state_changed(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    
    if (!ev) return ZMK_EV_EVENT_BUBBLE;
    
    /* 获取所有设备实例 */
    const struct device *dev = DEVICE_DT_GET_ANY(DT_DRV_COMPAT);
    if (!device_is_ready(dev)) return ZMK_EV_EVENT_BUBBLE;
    
    struct layer_indicator_data *data = dev->data;
    const struct layer_indicator_config *cfg = dev->config;
    
    k_mutex_lock(&data->lock, K_FOREVER);
    
    /* 更新层状态 */
    if (ev->layer < ZMK_KEYMAP_LAYERS_LEN) {
        data->layer_state[ev->layer] = ev->state;
    }
    
    uint8_t old_count = data->active_layers;
    update_layer_stats(data);
    
    /* 记录日志 */
    if (cfg->log_all_transitions || old_count != data->active_layers) {
        LOG_INF("Layer %d %s - Active: %d, Highest: %d",
               ev->layer, ev->state ? "ON" : "OFF", 
               data->active_layers, data->highest_layer);
    }
    
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

/* API: 获取激活层数 */
static int layer_indicator_get_count(const struct device *dev, uint8_t *count) {
    if (!device_is_ready(dev)) return -ENODEV;
    
    struct layer_indicator_data *data = dev->data;
    
    k_mutex_lock(&data->lock, K_FOREVER);
    if (count) *count = data->active_layers;
    k_mutex_unlock(&data->lock);
    
    return 0;
}

/* API: 获取最高层 */
static int layer_indicator_get_highest(const struct device *dev, uint8_t *layer) {
    if (!device_is_ready(dev)) return -ENODEV;
    
    struct layer_indicator_data *data = dev->data;
    
    k_mutex_lock(&data->lock, K_FOREVER);
    if (layer) *layer = data->highest_layer;
    k_mutex_unlock(&data->lock);
    
    return 0;
}

/* API: 检查层是否激活 */
static int layer_indicator_is_active(const struct device *dev, uint8_t layer, bool *active) {
    if (!device_is_ready(dev)) return -ENODEV;
    if (layer >= ZMK_KEYMAP_LAYERS_LEN) return -EINVAL;
    
    struct layer_indicator_data *data = dev->data;
    
    k_mutex_lock(&data->lock, K_FOREVER);
    if (active) *active = data->layer_state[layer];
    k_mutex_unlock(&data->lock);
    
    return 0;
}

/* API: 获取最后变化时间 */
static int layer_indicator_get_change_time(const struct device *dev, int64_t *time) {
    if (!device_is_ready(dev)) return -ENODEV;
    
    struct layer_indicator_data *data = dev->data;
    
    k_mutex_lock(&data->lock, K_FOREVER);
    if (time) *time = data->last_change;
    k_mutex_unlock(&data->lock);
    
    return 0;
}

/* API: 获取所有激活层 */
static int layer_indicator_get_all(const struct device *dev, 
                                  uint8_t *layers, uint8_t max, uint8_t *found) {
    if (!device_is_ready(dev)) return -ENODEV;
    
    struct layer_indicator_data *data = dev->data;
    
    k_mutex_lock(&data->lock, K_FOREVER);
    
    uint8_t count = 0;
    for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN && count < max; i++) {
        if (data->layer_state[i]) {
            if (layers) layers[count] = i;
            count++;
        }
    }
    
    if (found) *found = count;
    k_mutex_unlock(&data->lock);
    
    return 0;
}

/* 驱动初始化 */
static int layer_indicator_init(const struct device *dev) {
    struct layer_indicator_data *data = dev->data;
    
    k_mutex_init(&data->lock);
    
    k_mutex_lock(&data->lock, K_FOREVER);
    
    /* 初始化层状态 - 默认层0是激活的 */
    for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        data->layer_state[i] = (i == 0); /* 仅层0默认激活 */
    }
    
    update_layer_stats(data);
    k_mutex_unlock(&data->lock);
    
    LOG_INF("Layer indicator ready");
    return 0;
}

/* 驱动API */
static const struct layer_indicator_api {
    int (*get_count)(const struct device *dev, uint8_t *count);
    int (*get_highest)(const struct device *dev, uint8_t *layer);
    int (*is_active)(const struct device *dev, uint8_t layer, bool *active);
    int (*get_change_time)(const struct device *dev, int64_t *time);
    int (*get_all)(const struct device *dev, uint8_t *layers, uint8_t max, uint8_t *found);
} api = {
    .get_count = layer_indicator_get_count,
    .get_highest = layer_indicator_get_highest,
    .is_active = layer_indicator_is_active,
    .get_change_time = layer_indicator_get_change_time,
    .get_all = layer_indicator_get_all,
};

/* 事件监听 */
ZMK_LISTENER(layer_indicator, handle_layer_state_changed);
ZMK_SUBSCRIPTION(layer_indicator, zmk_layer_state_changed);

/* 设备实例化 */
#define LAYER_INDICATOR_INIT(n) \
    static struct layer_indicator_data data_##n; \
    static const struct layer_indicator_config cfg_##n = { \
        .log_all_transitions = DT_INST_PROP_OR(n, log_all_transitions, false), \
    }; \
    DEVICE_DT_INST_DEFINE(n, layer_indicator_init, NULL, &data_##n, &cfg_##n, \
                         POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(LAYER_INDICATOR_INIT)