/*
 * Copyright (c) 2025 ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

/* 
 * BLE_CHECK 驱动配置宏
 * 可以直接在编译时通过 -D 参数覆盖这些宏
 */
#ifndef BLE_CHECK_ENABLED
#define BLE_CHECK_ENABLED 1  /* 1=启用, 0=禁用 */
#endif

#ifndef BLE_CHECK_LOG_LEVEL
#define BLE_CHECK_LOG_LEVEL 3  /* 0=关闭, 1=错误, 2=警告, 3=信息, 4=调试 */
#endif

#ifndef BLE_CHECK_INIT_PRIORITY
#define BLE_CHECK_INIT_PRIORITY 90  /* 初始化优先级，确保在蓝牙初始化之后 */
#endif

/* LED闪烁配置 */
#ifndef BLE_CHECK_LED_BLINK_INTERVAL_MS
#define BLE_CHECK_LED_BLINK_INTERVAL_MS 500  /* 闪烁间隔（毫秒） */
#endif

/* 条件编译：仅在启用时编译驱动 */
#if BLE_CHECK_ENABLED

/* 创建日志模块实例 */
LOG_MODULE_REGISTER(ble_check, BLE_CHECK_LOG_LEVEL);

/* 设备树节点定义 - 直接使用节点，不使用别名 */
#define LED_NODE DT_PATH(ble_check_led)

/* 检查LED设备树节点是否存在 */
#if DT_NODE_HAS_STATUS(LED_NODE, okay)
#define LED_DEVICE_SUPPORTED 1
#else
#define LED_DEVICE_SUPPORTED 0
#warning "BLE check LED device tree node not defined. LED indication will be disabled."
#endif

/* GPIO设备结构体 */
#if LED_DEVICE_SUPPORTED
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#endif

/* 内部状态变量 */
static bool ble_check_initialized = false;
static bool last_connection_state = false;
static struct k_work_delayable led_blink_work;

/**
 * @brief LED闪烁工作函数
 */
static void led_blink_work_handler(struct k_work *work)
{
    if (!ble_check_initialized) {
        return;
    }

    bool is_connected = zmk_ble_active_profile_is_connected();
    
    if (!is_connected) {
        /* 未连接状态：切换LED状态 */
#if LED_DEVICE_SUPPORTED
        int ret = gpio_pin_toggle_dt(&led);
        if (ret < 0) {
            LOG_ERR("Failed to toggle LED: %d", ret);
        }
#endif
        
        /* 重新调度闪烁工作 */
        k_work_schedule(&led_blink_work, K_MSEC(BLE_CHECK_LED_BLINK_INTERVAL_MS));
    } else {
        /* 连接状态：LED熄灭 */
#if LED_DEVICE_SUPPORTED
        /* 高电平有效，所以输出0来熄灭LED */
        int ret = gpio_pin_set_dt(&led, 0);
        if (ret < 0) {
            LOG_ERR("Failed to set LED off: %d", ret);
        }
#endif
    }
}

/**
 * @brief 更新LED状态
 */
static void update_led_indication(bool is_connected)
{
    if (!ble_check_initialized) {
        return;
    }
    
    if (is_connected) {
        /* 连接状态：LED熄灭 */
        k_work_cancel_delayable(&led_blink_work);
#if LED_DEVICE_SUPPORTED
        int ret = gpio_pin_set_dt(&led, 0);
        if (ret < 0) {
            LOG_ERR("Failed to set LED off: %d", ret);
        }
#endif
        LOG_DBG("LED: OFF (connected)");
    } else {
        /* 未连接状态：LED闪烁 */
#if LED_DEVICE_SUPPORTED
        /* 先确保LED亮起 */
        int ret = gpio_pin_set_dt(&led, 1);
        if (ret < 0) {
            LOG_ERR("Failed to set LED on: %d", ret);
        }
#endif
        /* 启动闪烁工作 */
        k_work_schedule(&led_blink_work, K_MSEC(BLE_CHECK_LED_BLINK_INTERVAL_MS));
        LOG_DBG("LED: BLINKING (disconnected)");
    }
}

/**
 * @brief 检查蓝牙连接状态
 * 
 * @return true 蓝牙已连接
 * @return false 蓝牙未连接
 */
bool ble_check_is_connected(void)
{
    bool is_connected = zmk_ble_active_profile_is_connected();
    
    if (ble_check_initialized) {
        LOG_DBG("BLE connection status: %s", 
                is_connected ? "CONNECTED" : "DISCONNECTED");
    }
    
    return is_connected;
}

/**
 * @brief 处理蓝牙活跃profile变化事件
 */
static int ble_check_listener(const zmk_event_t *eh)
{
    struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);
    
    if (ev != NULL) {
        bool current_connected = zmk_ble_active_profile_is_connected();
        
        /* 只在状态变化时记录 */
        if (current_connected != last_connection_state) {
            LOG_INF("BLE connection state changed: %s -> %s",
                    last_connection_state ? "CONNECTED" : "DISCONNECTED",
                    current_connected ? "CONNECTED" : "DISCONNECTED");
            last_connection_state = current_connected;
            
            /* 更新LED指示 */
            update_led_indication(current_connected);
        }
        
        LOG_DBG("Active profile changed to %d", ev->index);
    }
    
    return 0;
}

/* ZMK事件订阅必须在全局作用域 */
ZMK_LISTENER(ble_check, ble_check_listener);
ZMK_SUBSCRIPTION(ble_check, zmk_ble_active_profile_changed);

/**
 * @brief 初始化 BLE_CHECK 驱动
 */
static int ble_check_init(void)
{
    /* 检查宏配置 */
    LOG_DBG("BLE_CHECK configuration:");
    LOG_DBG("  - BLE_CHECK_ENABLED: %d", BLE_CHECK_ENABLED);
    LOG_DBG("  - BLE_CHECK_LOG_LEVEL: %d", BLE_CHECK_LOG_LEVEL);
    LOG_DBG("  - BLE_CHECK_INIT_PRIORITY: %d", BLE_CHECK_INIT_PRIORITY);
    LOG_DBG("  - BLE_CHECK_LED_BLINK_INTERVAL_MS: %d", BLE_CHECK_LED_BLINK_INTERVAL_MS);
    
    /* 初始化LED闪烁工作 */
    k_work_init_delayable(&led_blink_work, led_blink_work_handler);
    
#if LED_DEVICE_SUPPORTED
    /* 初始化GPIO LED */
    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device is not ready");
        return -ENODEV;
    }
    
    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED: %d", ret);
        return ret;
    }
    
    LOG_INF("LED configured on GPIO: port=%p, pin=%d, dt_flags=0x%x", 
            led.port, led.pin, led.dt_flags);
#else
    LOG_WRN("LED device tree node not found. LED indication disabled.");
#endif
    
    /* 获取初始连接状态 */
    last_connection_state = zmk_ble_active_profile_is_connected();
    
    LOG_INF("BLE check driver initialized");
    LOG_INF("Initial connection state: %s", 
            last_connection_state ? "CONNECTED" : "DISCONNECTED");
    
    /* 初始LED状态 */
    update_led_indication(last_connection_state);
    
    ble_check_initialized = true;
    
    return 0;
}

/* 初始化驱动 */
SYS_INIT(ble_check_init, APPLICATION, BLE_CHECK_INIT_PRIORITY);

#else /* BLE_CHECK_ENABLED == 0 */

/* 当驱动被禁用时的空实现 */
bool ble_check_is_connected(void)
{
    return false;  /* 默认返回未连接 */
}

#endif /* BLE_CHECK_ENABLED */