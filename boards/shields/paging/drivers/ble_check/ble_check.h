/*
 * Copyright (c) 2025 ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE_CHECK 驱动配置宏
 * 
 * 可以在编译时通过 -D 参数覆盖这些默认值：
 * -DBLE_CHECK_ENABLED=1
 * -DBLE_CHECK_LOG_LEVEL=4
 * -DBLE_CHECK_INIT_PRIORITY=90
 * -DBLE_CHECK_LED_PIN=5          # P0.05
 * -DBLE_CHECK_LED_BLINK_INTERVAL_MS=500
 */

/**
 * @brief 检查蓝牙连接状态
 * 
 * @return true 蓝牙已连接
 * @return false 蓝牙未连接
 */
bool ble_check_is_connected(void);

#ifdef __cplusplus
}
#endif