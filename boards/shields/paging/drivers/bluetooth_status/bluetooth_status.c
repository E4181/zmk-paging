/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Kconfig配置宏定义 */
#ifndef CONFIG_BLUETOOTH_STATUS_LED_PIN
#define CONFIG_BLUETOOTH_STATUS_LED_PIN 26  /* 默认使用 P0.26 */
#endif

#ifndef CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS
#define CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS 500  /* 默认闪烁间隔 500ms */
#endif

/* 设备私有数据结构 */
struct bluetooth_status_data {
    const struct device *gpio_dev;
    bool led_state;
    bool is_connected;
    uint64_t last_blink_time;
};

/* 全局实例 */
static struct bluetooth_status_data bluetooth_data;

/* 定时器回调函数 */
static void bluetooth_status_timer_handler(struct k_timer *timer)
{
    bool connected = zmk_ble_active_profile_is_connected();
    uint64_t current_time = k_uptime_get();
    
    /* 如果连接状态发生变化 */
    if (connected != bluetooth_data.is_connected) {
        bluetooth_data.is_connected = connected;
        
        if (connected) {
            /* 连接成功，熄灭LED */
            gpio_pin_set(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, 0);
            bluetooth_data.led_state = false;
        } else {
            /* 断开连接，开始闪烁 */
            gpio_pin_set(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, 1);
            bluetooth_data.led_state = true;
            bluetooth_data.last_blink_time = current_time;
        }
    }
    
    /* 如果未连接，处理闪烁 */
    if (!bluetooth_data.is_connected) {
        if (current_time - bluetooth_data.last_blink_time >= CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS) {
            /* 切换LED状态 */
            bluetooth_data.led_state = !bluetooth_data.led_state;
            gpio_pin_set(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, 
                        bluetooth_data.led_state ? 1 : 0);
            bluetooth_data.last_blink_time = current_time;
        }
    }
}

/* 定义定时器 */
K_TIMER_DEFINE(bluetooth_status_timer, bluetooth_status_timer_handler, NULL);

/* 初始化函数 */
static int bluetooth_status_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    
    /* 初始化设备数据 */
    memset(&bluetooth_data, 0, sizeof(bluetooth_data));
    
    /* 获取GPIO设备 */
    bluetooth_data.gpio_dev = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
    if (!bluetooth_data.gpio_dev) {
        LOG_ERR("Failed to get GPIO device");
        return -ENODEV;
    }
    
    /* 配置GPIO引脚 */
    int ret = gpio_pin_configure(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, 
                                GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_HIGH);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin (err %d)", ret);
        return ret;
    }
    
    /* 初始化为熄灭状态 */
    gpio_pin_set(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, 0);
    bluetooth_data.is_connected = zmk_ble_active_profile_is_connected();
    bluetooth_data.last_blink_time = k_uptime_get();
    
    /* 启动定时器，每100ms检查一次 */
    k_timer_start(&bluetooth_status_timer, K_MSEC(100), K_MSEC(100));
    
    LOG_INF("Bluetooth status indicator initialized");
    return 0;
}

/* 设备定义 */
SYS_INIT(bluetooth_status_init, APPLICATION, 90);