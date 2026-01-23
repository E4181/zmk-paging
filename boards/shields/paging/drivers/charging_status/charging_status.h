/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * Charging Status Driver API for TP4056
 */

#ifndef ZEPHYR_DRIVERS_CHARGING_STATUS_H_
#define ZEPHYR_DRIVERS_CHARGING_STATUS_H_

#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 获取当前充电状态
 * @param dev 充电状态设备指针
 * @param is_charging 存储充电状态的指针（true=充电，false=未充电）
 * @return 0表示成功，负数表示错误码
 */
int charging_status_get(const struct device *dev, bool *is_charging);

/**
 * @brief 注册充电状态变化回调函数
 * @param dev 充电状态设备指针
 * @param callback 回调函数指针
 * @param user_data 传递给回调函数的用户数据
 * @return 0表示成功，负数表示错误码
 */
int charging_status_register_callback(const struct device *dev,
                                    void (*callback)(bool is_charging, void *user_data),
                                    void *user_data);

/**
 * @brief 获取最后一次状态变化的时间戳
 * @param dev 充电状态设备指针
 * @param timestamp 存储时间戳的指针（毫秒自启动）
 * @return 0表示成功，负数表示错误码
 */
int charging_status_get_last_change(const struct device *dev,
                                  int64_t *timestamp);

/**
 * @brief 获取驱动统计信息
 * @param dev 充电状态设备指针
 * @param last_change_time 最后状态变化时间
 * @param change_count 状态变化次数
 * @param interrupt_count 中断触发次数
 * @param hardware_fault 硬件故障标志
 * @return 0表示成功，负数表示错误码
 */
int charging_status_get_stats(const struct device *dev,
                            int64_t *last_change_time,
                            uint32_t *change_count,
                            uint32_t *interrupt_count,
                            bool *hardware_fault);

/**
 * @brief 获取当前GPIO状态（调试用）
 * @param dev 充电状态设备指针
 * @param raw_level 存储原始物理电平（0=低电平，1=高电平）
 * @param logical_level 存储逻辑电平（考虑GPIO_ACTIVE_LOW标志）
 * @return 0表示成功，负数表示错误码
 */
int charging_status_get_gpio_state(const struct device *dev,
                                 int *raw_level,
                                 int *logical_level);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_CHARGING_STATUS_H_ */