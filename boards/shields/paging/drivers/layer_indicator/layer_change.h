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

/* 层变化LED控制API */
int layer_change_enable(const struct device *dev);
int layer_change_disable(const struct device *dev);
bool layer_change_is_enabled(const struct device *dev);

#ifdef __cplusplus
}
#endif