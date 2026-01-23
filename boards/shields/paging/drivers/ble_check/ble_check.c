/*
 * Copyright (c) 2025 ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
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

/* 条件编译：仅在启用时编译驱动 */
#if BLE_CHECK_ENABLED

/* 创建日志模块实例 */
LOG_MODULE_REGISTER(ble_check, BLE_CHECK_LOG_LEVEL);

/* 内部状态变量 */
static bool ble_check_initialized = false;
static bool last_connection_state = false;

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
        }
        
        LOG_DBG("Active profile changed to %d", ev->index);
    }
    
    return 0;
}

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
    
    /* 订阅蓝牙活跃profile变化事件 */
    ZMK_SUBSCRIPTION(ble_check, zmk_ble_active_profile_changed);
    
    /* 获取初始连接状态 */
    last_connection_state = zmk_ble_active_profile_is_connected();
    
    LOG_INF("BLE check driver initialized");
    LOG_INF("Initial connection state: %s", 
            last_connection_state ? "CONNECTED" : "DISCONNECTED");
    
    ble_check_initialized = true;
    
    return 0;
}

/* 注册事件监听器 */
ZMK_LISTENER(ble_check, ble_check_listener);

/* 初始化驱动 */
SYS_INIT(ble_check_init, APPLICATION, BLE_CHECK_INIT_PRIORITY);

#else /* BLE_CHECK_ENABLED == 0 */

/* 当驱动被禁用时的空实现 */
bool ble_check_is_connected(void)
{
    return false;  /* 默认返回未连接 */
}

#endif /* BLE_CHECK_ENABLED */