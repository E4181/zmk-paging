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

/* LED控制函数 */
static int set_led_state(bool state)
{
    int ret;
    
    if (!bluetooth_data.gpio_dev) {
        return -ENODEV;
    }
    
    ret = gpio_pin_set(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, state ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to set LED state (err %d)", ret);
        return ret;
    }
    
    bluetooth_data.led_state = state;
    return 0;
}

/* 检查蓝牙连接状态并更新LED */
static void check_and_update_led(void)
{
    bool connected = zmk_ble_active_profile_is_connected();
    uint64_t current_time = k_uptime_get();
    
    /* 如果连接状态发生变化 */
    if (connected != bluetooth_data.is_connected) {
        bluetooth_data.is_connected = connected;
        
        if (connected) {
            /* 连接成功，熄灭LED */
            set_led_state(false);
            LOG_INF("Bluetooth connected, LED off");
        } else {
            /* 断开连接，开始闪烁 */
            set_led_state(true);  /* 先点亮 */
            bluetooth_data.last_blink_time = current_time;
            LOG_INF("Bluetooth disconnected, LED blinking");
        }
    }
    
    /* 如果未连接，处理闪烁 */
    if (!bluetooth_data.is_connected) {
        if (current_time - bluetooth_data.last_blink_time >= CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS) {
            /* 切换LED状态 */
            set_led_state(!bluetooth_data.led_state);
            bluetooth_data.last_blink_time = current_time;
        }
    }
}

/* 定时器回调函数 */
static void bluetooth_status_timer_handler(struct k_timer *timer)
{
    check_and_update_led();
}

/* 定义定时器 */
K_TIMER_DEFINE(bluetooth_status_timer, bluetooth_status_timer_handler, NULL);

/* 初始化函数 */
static int bluetooth_status_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    
    /* 初始化设备数据 */
    memset(&bluetooth_data, 0, sizeof(bluetooth_data));
    
    /* 获取GPIO设备 - 使用正确的设备树节点引用 */
    bluetooth_data.gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(bluetooth_data.gpio_dev)) {
        LOG_ERR("GPIO device not ready");
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
    
    /* 检查初始连接状态 */
    bluetooth_data.is_connected = zmk_ble_active_profile_is_connected();
    bluetooth_data.last_blink_time = k_uptime_get();
    
    /* 根据初始状态设置LED */
    if (bluetooth_data.is_connected) {
        set_led_state(false);  /* 已连接，LED熄灭 */
        LOG_INF("Initial state: Bluetooth connected, LED off");
    } else {
        set_led_state(true);   /* 未连接，LED点亮（开始闪烁的第一步） */
        LOG_INF("Initial state: Bluetooth disconnected, LED blinking");
    }
    
    /* 启动定时器，每100ms检查一次 */
    k_timer_start(&bluetooth_status_timer, K_MSEC(100), K_MSEC(100));
    
    LOG_INF("Bluetooth status indicator initialized");
    return 0;
}

/* 设备定义 */
SYS_INIT(bluetooth_status_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);