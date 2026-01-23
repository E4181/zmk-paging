/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_layer_indicator

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <stdio.h>  /* 添加这个头文件 */
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>

LOG_MODULE_REGISTER(layer_indicator, CONFIG_ZMK_LOG_LEVEL);

/* 如果没有定义，使用默认值 */
#ifndef CONFIG_ZMK_LAYER_INDICATOR_MAX_EVENTS
#define CONFIG_ZMK_LAYER_INDICATOR_MAX_EVENTS 20
#endif

/* 驱动配置结构体 */
struct layer_indicator_config {
    bool log_all_transitions;  /* 是否记录所有层状态变化 */
    uint32_t max_active_layers; /* 最大激活层数记录 */
};

/* 驱动状态结构体 */
struct layer_indicator_data {
    const struct device *dev;
    struct k_mutex lock;
    uint8_t active_layer_count;   /* 当前激活的层数 */
    uint8_t highest_active_layer; /* 当前最高激活层 */
    uint8_t previous_layer_count; /* 上一次激活的层数 */
    int64_t last_change_timestamp; /* 最后一次层变化的时间戳 */
    
    /* 记录每个层的状态 */
    bool layer_states[ZMK_KEYMAP_LAYERS_LEN];
};

/* 消息队列用于处理层状态变化事件 */
struct layer_indicator_event {
    uint8_t layer;
    bool state;
    int64_t timestamp;
};

K_MSGQ_DEFINE(layer_indicator_msgq, 
              sizeof(struct layer_indicator_event),
              CONFIG_ZMK_LAYER_INDICATOR_MAX_EVENTS,
              4);

/* 工作队列回调函数 */
static void process_layer_events_cb(struct k_work *work) {
    struct layer_indicator_event event;
    const struct device *dev = DEVICE_DT_INST_GET(0);
    
    if (!device_is_ready(dev)) {
        LOG_ERR("Device not ready");
        return;
    }
    
    struct layer_indicator_data *data = dev->data;
    const struct layer_indicator_config *cfg = dev->config;
    
    while (k_msgq_get(&layer_indicator_msgq, &event, K_NO_WAIT) >= 0) {
        int ret = k_mutex_lock(&data->lock, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("Failed to lock mutex: %d", ret);
            continue;
        }
        
        /* 更新层状态数组 */
        if (event.layer < ZMK_KEYMAP_LAYERS_LEN) {
            data->layer_states[event.layer] = event.state;
        }
        
        /* 重新计算激活层数 */
        uint8_t new_count = 0;
        uint8_t highest_layer = 0;
        
        for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
            if (data->layer_states[i]) {
                new_count++;
                if (i > highest_layer) {
                    highest_layer = i;
                }
            }
        }
        
        /* 保存旧值用于比较 */
        data->previous_layer_count = data->active_layer_count;
        data->active_layer_count = new_count;
        data->highest_active_layer = highest_layer;
        data->last_change_timestamp = event.timestamp;
        
        /* 记录日志 */
        if (cfg->log_all_transitions || 
            data->previous_layer_count != data->active_layer_count) {
            
            LOG_INF("Layer %d %s (Active layers: %d, Highest: %d, Time: %lld)",
                   event.layer,
                   event.state ? "activated" : "deactivated",
                   data->active_layer_count,
                   data->highest_active_layer,
                   event.timestamp);
            
            /* 记录所有激活的层 */
            if (data->active_layer_count > 0) {
                char layer_list[64] = {0};
                int pos = 0;
                
                for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
                    if (data->layer_states[i]) {
                        pos += snprintf(layer_list + pos, sizeof(layer_list) - pos, 
                                       "%d ", i);
                        if (pos >= sizeof(layer_list)) break;
                    }
                }
                
                if (pos > 0) {
                    LOG_DBG("Active layers: %s", layer_list);
                }
            } else {
                LOG_DBG("No active layers (default layer only)");
            }
        }
        
        k_mutex_unlock(&data->lock);
    }
}

static K_WORK_DEFINE(layer_indicator_work, process_layer_events_cb);

/* 层状态变化事件处理函数 */
static int handle_layer_state_changed(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    /* 创建事件并放入消息队列 */
    struct layer_indicator_event event = {
        .layer = ev->layer,
        .state = ev->state,
        .timestamp = ev->timestamp
    };
    
    int ret = k_msgq_put(&layer_indicator_msgq, &event, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("Layer indicator message queue full, dropping event");
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    /* 提交工作队列处理 */
    k_work_submit(&layer_indicator_work);
    
    return ZMK_EV_EVENT_BUBBLE;
}

/* 获取当前激活层数的API函数 */
static int layer_indicator_get_active_count(const struct device *dev, uint8_t *count) {
    if (!device_is_ready(dev)) {
        LOG_ERR("Device not ready");
        return -ENODEV;
    }
    
    struct layer_indicator_data *data = dev->data;
    
    if (count == NULL) {
        return -EINVAL;
    }
    
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    
    *count = data->active_layer_count;
    
    k_mutex_unlock(&data->lock);
    
    LOG_DBG("API call: get_active_count = %d", *count);
    return 0;
}

/* 获取最高激活层的API函数 */
static int layer_indicator_get_highest_layer(const struct device *dev, uint8_t *layer) {
    if (!device_is_ready(dev)) {
        LOG_ERR("Device not ready");
        return -ENODEV;
    }
    
    struct layer_indicator_data *data = dev->data;
    
    if (layer == NULL) {
        return -EINVAL;
    }
    
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    
    *layer = data->highest_active_layer;
    
    k_mutex_unlock(&data->lock);
    
    LOG_DBG("API call: get_highest_layer = %d", *layer);
    return 0;
}

/* 检查特定层是否激活的API函数 */
static int layer_indicator_is_layer_active(const struct device *dev, uint8_t layer, bool *active) {
    if (!device_is_ready(dev)) {
        LOG_ERR("Device not ready");
        return -ENODEV;
    }
    
    struct layer_indicator_data *data = dev->data;
    
    if (active == NULL || layer >= ZMK_KEYMAP_LAYERS_LEN) {
        return -EINVAL;
    }
    
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    
    *active = data->layer_states[layer];
    
    k_mutex_unlock(&data->lock);
    
    LOG_DBG("API call: is_layer_active(%d) = %s", layer, *active ? "true" : "false");
    return 0;
}

/* 获取所有激活层的API函数 */
static int layer_indicator_get_all_active(const struct device *dev, 
                                         uint8_t *layers, 
                                         uint8_t max_layers, 
                                         uint8_t *count) {
    if (!device_is_ready(dev)) {
        LOG_ERR("Device not ready");
        return -ENODEV;
    }
    
    struct layer_indicator_data *data = dev->data;
    
    if (layers == NULL || count == NULL) {
        return -EINVAL;
    }
    
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    
    uint8_t found = 0;
    for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN && found < max_layers; i++) {
        if (data->layer_states[i]) {
            layers[found++] = i;
        }
    }
    
    *count = found;
    
    k_mutex_unlock(&data->lock);
    
    LOG_DBG("API call: get_all_active found %d layers", found);
    return 0;
}

/* 获取最后变化时间的API函数 */
static int layer_indicator_get_last_change_time(const struct device *dev, int64_t *timestamp) {
    if (!device_is_ready(dev)) {
        LOG_ERR("Device not ready");
        return -ENODEV;
    }
    
    struct layer_indicator_data *data = dev->data;
    
    if (timestamp == NULL) {
        return -EINVAL;
    }
    
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    
    *timestamp = data->last_change_timestamp;
    
    k_mutex_unlock(&data->lock);
    
    LOG_DBG("API call: get_last_change_time = %lld", *timestamp);
    return 0;
}

/* 驱动初始化函数 */
static int layer_indicator_init(const struct device *dev) {
    struct layer_indicator_data *data = dev->data;
    
    /* 初始化互斥锁 */
    k_mutex_init(&data->lock);
    
    /* 初始化层状态数组 */
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Failed to lock mutex during init");
        return ret;
    }
    
    for (int i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        data->layer_states[i] = false;
    }
    
    /* 默认层总是激活的 */
    data->layer_states[0] = true;
    data->active_layer_count = 1;
    data->highest_active_layer = 0;
    data->previous_layer_count = 1;
    data->last_change_timestamp = k_uptime_get();
    
    k_mutex_unlock(&data->lock);
    
    LOG_INF("Layer indicator initialized (max layers: %d)", ZMK_KEYMAP_LAYERS_LEN);
    LOG_DBG("Default layer 0 activated on startup");
    
    return 0;
}

/* 驱动API结构体 */
struct layer_indicator_api {
    int (*get_active_count)(const struct device *dev, uint8_t *count);
    int (*get_highest_layer)(const struct device *dev, uint8_t *layer);
    int (*is_layer_active)(const struct device *dev, uint8_t layer, bool *active);
    int (*get_all_active)(const struct device *dev, uint8_t *layers, uint8_t max_layers, uint8_t *count);
    int (*get_last_change_time)(const struct device *dev, int64_t *timestamp);
};

static const struct layer_indicator_api layer_indicator_api_funcs = {
    .get_active_count = layer_indicator_get_active_count,
    .get_highest_layer = layer_indicator_get_highest_layer,
    .is_layer_active = layer_indicator_is_layer_active,
    .get_all_active = layer_indicator_get_all_active,
    .get_last_change_time = layer_indicator_get_last_change_time,
};

/* 事件监听器注册 */
ZMK_LISTENER(layer_indicator, handle_layer_state_changed);
ZMK_SUBSCRIPTION(layer_indicator, zmk_layer_state_changed);

/* 设备实例化 */
#define LAYER_INDICATOR_INIT(n) \
    static struct layer_indicator_data layer_indicator_data_##n = { \
        .dev = DEVICE_DT_INST_GET(n), \
    }; \
    \
    static const struct layer_indicator_config layer_indicator_config_##n = { \
        .log_all_transitions = DT_INST_PROP_OR(n, log_all_transitions, false), \
        .max_active_layers = DT_INST_PROP_OR(n, max_active_layers, ZMK_KEYMAP_LAYERS_LEN), \
    }; \
    \
    DEVICE_DT_INST_DEFINE(n, \
                         layer_indicator_init, \
                         NULL, \
                         &layer_indicator_data_##n, \
                         &layer_indicator_config_##n, \
                         POST_KERNEL, \
                         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                         &layer_indicator_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(LAYER_INDICATOR_INIT)