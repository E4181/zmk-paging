/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取当前蓝牙连接状态
 * @return true 已连接，false 未连接
 */
bool bluetooth_status_is_connected(void);

/**
 * @brief 手动更新蓝牙状态指示
 * @note 通常不需要手动调用，系统会自动处理
 */
void bluetooth_status_update(void);

#ifdef __cplusplus
}
#endif