/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * Charging Status Driver API for TP4056 with LED indication
 */

#ifndef ZEPHYR_DRIVERS_CHARGING_STATUS_H_
#define ZEPHYR_DRIVERS_CHARGING_STATUS_H_

#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 充电状态相关API ============ */

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

/* ============ LED控制API ============ */

/**
 * @brief 手动控制LED（测试用）
 * @param dev 充电状态设备指针
 * @param enable 是否启用LED
 * @return 0表示成功，负数表示错误码
 */
int charging_status_led_set(const struct device *dev, bool enable);

/**
 * @brief 获取当前LED状态
 * @param dev 充电状态设备指针
 * @param is_active 存储LED状态的指针（true=活动，false=熄灭）
 * @return 0表示成功，负数表示错误码
 */
int charging_status_led_get_state(const struct device *dev, bool *is_active);

/**
 * @brief 设置LED效果参数
 * @param dev 充电状态设备指针
 * @param period_ms 呼吸周期（毫秒，PWM模式）
 * @param max_brightness 最大亮度（0-255，PWM模式）
 * @param blink_interval_ms 闪烁间隔（毫秒，GPIO模式）
 * @return 0表示成功，负数表示错误码
 */
int charging_status_led_set_params(const struct device *dev,
                                 uint32_t period_ms,
                                 uint8_t max_brightness,
                                 uint32_t blink_interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_CHARGING_STATUS_H_ */