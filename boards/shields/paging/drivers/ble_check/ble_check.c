/*
 * Copyright (c) 2025 ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <nrfx.h>
#include <hal/nrf_gpio.h>

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

/* LED引脚配置 - 硬编码为P0.05 */
#ifndef BLE_CHECK_LED_PIN
#define BLE_CHECK_LED_PIN 5  /* P0.05 */
#endif

#ifndef BLE_CHECK_LED_BLINK_INTERVAL_MS
#define BLE_CHECK_LED_BLINK_INTERVAL_MS 500  /* 闪烁间隔（毫秒） */
#endif

/* 条件编译：仅在启用时编译驱动 */
#if BLE_CHECK_ENABLED

/* 创建日志模块实例 */
LOG_MODULE_REGISTER(ble_check, BLE_CHECK_LOG_LEVEL);

/* 内部状态变量 */
static bool ble_check_initialized = false;
static bool last_connection_state = false;
static struct k_work_delayable led_blink_work;
static bool led_blink_state = false;
static bool led_enabled = true;  /* LED指示是否启用 */

/**
 * @brief 直接操作NRF52840 GPIO引脚
 * 
 * @param pin 引脚号
 * @param value 输出值: 0=低电平, 1=高电平
 */
static void ble_check_led_write(uint32_t pin, uint32_t value)
{
    if (value) {
        nrf_gpio_pin_set(pin);
    } else {
        nrf_gpio_pin_clear(pin);
    }
}

/**
 * @brief 初始化NRF52840 GPIO引脚
 * 
 * @param pin 引脚号
 * @return int 0=成功，负值=错误
 */
static int ble_check_led_init(uint32_t pin)
{
    /* 检查引脚号是否有效 */
    if (pin > 31) {
        LOG_ERR("Invalid pin number: %d. Must be 0-31 for P0.x", pin);
        led_enabled = false;
        return -EINVAL;
    }
    
    /* 配置引脚为输出模式 */
    nrf_gpio_cfg_output(pin);
    
    /* 初始状态为低电平（LED熄灭） */
    ble_check_led_write(pin, 0);
    
    LOG_INF("LED configured on P0.%d using direct NRF52840 GPIO control", pin);
    return 0;
}

/**
 * @brief LED闪烁工作函数
 */
static void led_blink_work_handler(struct k_work *work)
{
    if (!ble_check_initialized || !led_enabled) {
        return;
    }

    bool is_connected = zmk_ble_active_profile_is_connected();
    
    if (!is_connected) {
        /* 未连接状态：切换LED状态 */
        led_blink_state = !led_blink_state;
        ble_check_led_write(BLE_CHECK_LED_PIN, led_blink_state);
        
        /* 重新调度闪烁工作 */
        k_work_schedule(&led_blink_work, K_MSEC(BLE_CHECK_LED_BLINK_INTERVAL_MS));
    } else {
        /* 连接状态：LED熄灭（高电平有效，输出0） */
        led_blink_state = false;
        ble_check_led_write(BLE_CHECK_LED_PIN, 0);
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
        if (led_enabled) {
            ble_check_led_write(BLE_CHECK_LED_PIN, 0);
        }
        LOG_DBG("LED: OFF (connected)");
    } else {
        /* 未连接状态：LED闪烁 */
        if (led_enabled) {
            /* 先确保LED亮起 */
            ble_check_led_write(BLE_CHECK_LED_PIN, 1);
            /* 启动闪烁工作 */
            k_work_schedule(&led_blink_work, K_MSEC(BLE_CHECK_LED_BLINK_INTERVAL_MS));
        }
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
    LOG_DBG("  - BLE_CHECK_LED_PIN: %d", BLE_CHECK_LED_PIN);
    LOG_DBG("  - BLE_CHECK_LED_BLINK_INTERVAL_MS: %d", BLE_CHECK_LED_BLINK_INTERVAL_MS);
    
    /* 初始化LED闪烁工作 */
    k_work_init_delayable(&led_blink_work, led_blink_work_handler);
    
    /* 初始化LED引脚 */
    int ret = ble_check_led_init(BLE_CHECK_LED_PIN);
    if (ret < 0) {
        LOG_WRN("Failed to initialize LED pin, continuing without LED indication");
        led_enabled = false;
    }
    
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