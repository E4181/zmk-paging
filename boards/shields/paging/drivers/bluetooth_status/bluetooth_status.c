/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Kconfig配置宏定义 - 直接写在驱动文件中 */
#ifndef CONFIG_BLUETOOTH_STATUS_ENABLED
#define CONFIG_BLUETOOTH_STATUS_ENABLED 1
#endif

#ifndef CONFIG_BLUETOOTH_STATUS_LED_PIN
#define CONFIG_BLUETOOTH_STATUS_LED_PIN 26  /* 默认使用 P0.26 */
#endif

#ifndef CONFIG_BLUETOOTH_STATUS_LED_PORT
#define CONFIG_BLUETOOTH_STATUS_LED_PORT "GPIO_0"
#endif

#ifndef CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS
#define CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS 500  /* 默认闪烁间隔 500ms */
#endif

/* 设备私有数据结构 */
struct bluetooth_status_data {
    bool led_state;
    bool ble_initialized;
    bool is_connected;
    const struct device *gpio_dev;
    struct k_work_delayable led_work;
    struct k_work status_check_work;
};

/* 全局实例 */
static struct bluetooth_status_data bluetooth_data;

/* LED控制函数 */
static int set_led_state(bool state)
{
    int ret;
    
    if (!bluetooth_data.gpio_dev) {
        LOG_ERR("GPIO device not initialized");
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

/* 获取当前蓝牙连接状态 */
static bool check_bluetooth_connected(void)
{
    if (!bluetooth_data.ble_initialized) {
        return false;
    }
    
    return zmk_ble_active_profile_is_connected();
}

/* LED闪烁工作函数 */
static void led_blink_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct bluetooth_status_data *data = CONTAINER_OF(dwork, struct bluetooth_status_data, led_work);
    
    /* 检查蓝牙连接状态 */
    bool connected = check_bluetooth_connected();
    
    if (connected) {
        /* 连接成功，熄灭LED */
        set_led_state(false);
        data->is_connected = true;
        /* 停止定时器 */
        k_work_cancel_delayable(dwork);
        LOG_DBG("Bluetooth connected, LED off");
    } else {
        /* 未连接，切换LED状态实现闪烁 */
        set_led_state(!data->led_state);
        data->is_connected = false;
        /* 继续定时闪烁 */
        k_work_reschedule(dwork, K_MSEC(CONFIG_BLUETOOTH_STATUS_BLINK_INTERVAL_MS));
    }
}

/* 状态检查工作函数 */
static void status_check_work_handler(struct k_work *work)
{
    struct bluetooth_status_data *data = CONTAINER_OF(work, struct bluetooth_status_data, status_check_work);
    
    /* 检查蓝牙连接状态 */
    bool connected = check_bluetooth_connected();
    
    if (connected != data->is_connected) {
        /* 连接状态改变，更新LED状态 */
        if (connected) {
            /* 连接成功，熄灭LED */
            set_led_state(false);
            data->is_connected = true;
            /* 停止闪烁定时器 */
            k_work_cancel_delayable(&data->led_work);
        } else {
            /* 断开连接，开始闪烁 */
            data->is_connected = false;
            if (!k_work_delayable_is_pending(&data->led_work)) {
                k_work_schedule(&data->led_work, K_MSEC(0));
            }
        }
        LOG_DBG("Bluetooth connection status changed: %s", connected ? "connected" : "disconnected");
    }
}

/* 事件处理函数 */
static int bluetooth_status_event_listener(const zmk_event_t *eh)
{
    /* 处理蓝牙配置文件变更事件 */
    struct zmk_ble_active_profile_changed *profile_event = as_zmk_ble_active_profile_changed(eh);
    if (profile_event != NULL) {
        LOG_DBG("BLE profile changed, index: %d", profile_event->index);
        /* 提交工作以更新LED状态 */
        k_work_submit(&bluetooth_data.status_check_work);
        return 0;
    }
    
    return 0;
}

/* 初始化函数 */
static int bluetooth_status_init(const struct device *dev)
{
    int ret;
    
    /* 初始化设备数据 */
    memset(&bluetooth_data, 0, sizeof(bluetooth_data));
    bluetooth_data.led_state = false;
    bluetooth_data.ble_initialized = true;  /* 假设蓝牙已初始化 */
    bluetooth_data.is_connected = false;
    
    /* 获取GPIO设备 */
    bluetooth_data.gpio_dev = device_get_binding(CONFIG_BLUETOOTH_STATUS_LED_PORT);
    if (!bluetooth_data.gpio_dev) {
        LOG_ERR("Failed to get GPIO device for port %s", CONFIG_BLUETOOTH_STATUS_LED_PORT);
        return -ENODEV;
    }
    
    /* 配置GPIO引脚 */
    ret = gpio_pin_configure(bluetooth_data.gpio_dev, CONFIG_BLUETOOTH_STATUS_LED_PIN, 
                            GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_HIGH);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin (err %d)", ret);
        return ret;
    }
    
    /* 初始化为熄灭状态 */
    set_led_state(false);
    
    /* 初始化工作项 */
    k_work_init_delayable(&bluetooth_data.led_work, led_blink_work_handler);
    k_work_init(&bluetooth_data.status_check_work, status_check_work_handler);
    
    /* 检查初始连接状态 */
    bluetooth_data.is_connected = check_bluetooth_connected();
    
    if (!bluetooth_data.is_connected) {
        /* 未连接，启动闪烁 */
        k_work_schedule(&bluetooth_data.led_work, K_MSEC(0));
        LOG_INF("Bluetooth disconnected, LED blinking");
    } else {
        /* 已连接，LED熄灭 */
        set_led_state(false);
        LOG_INF("Bluetooth connected, LED off");
    }
    
    LOG_INF("Bluetooth status indicator initialized");
    return 0;
}

/* 订阅事件 */
ZMK_LISTENER(bluetooth_status, bluetooth_status_event_listener);
ZMK_SUBSCRIPTION(bluetooth_status, zmk_ble_active_profile_changed);

/* 设备定义 */
DEVICE_DEFINE(bluetooth_status, "bluetooth_status", 
              bluetooth_status_init, NULL, NULL, NULL,
              POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);