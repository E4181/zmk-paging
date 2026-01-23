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

/* 获取设备指针的宏 */
#define BLUETOOTH_STATUS_DEVICE DEVICE_DT_GET_OR_NULL(DT_ALIAS(bluetooth_status_led))

#ifdef __cplusplus
}
#endif