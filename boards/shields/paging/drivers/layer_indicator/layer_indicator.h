/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 获取激活层数 */
int layer_indicator_get_count(const struct device *dev, uint8_t *count);

/* 获取最高激活层 */
int layer_indicator_get_highest(const struct device *dev, uint8_t *layer);

/* 检查层是否激活 */
int layer_indicator_is_active(const struct device *dev, uint8_t layer, bool *active);

/* 获取所有激活层 */
int layer_indicator_get_all(const struct device *dev, 
                           uint8_t *layers, uint8_t max_layers, uint8_t *count);

/* 获取最后变化时间 */
int layer_indicator_get_change_time(const struct device *dev, int64_t *timestamp);

#ifdef __cplusplus
}
#endif