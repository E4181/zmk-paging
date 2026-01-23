/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 设备树配置宏 */
#define BLUETOOTH_STATUS_NODE DT_ALIAS(bluetooth_status)
#define BLUETOOTH_STATUS_LABEL DT_LABEL(BLUETOOTH_STATUS_NODE)

/* 获取设备指针的宏 */
#define BLUETOOTH_STATUS_DEVICE DEVICE_DT_GET_OR_NULL(BLUETOOTH_STATUS_NODE)

/* 公开的API函数 */
int bluetooth_status_set_pattern(uint8_t pattern);
int bluetooth_status_get_state(void);
int bluetooth_status_enable(bool enable);

#ifdef __cplusplus
}
#endif