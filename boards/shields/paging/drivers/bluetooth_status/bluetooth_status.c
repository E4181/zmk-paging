/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 检查设备树节点是否存在 */
#define BLUETOOTH_STATUS_NODE DT_PATH(bluetooth_status)

#if DT_NODE_EXISTS(BLUETOOTH_STATUS_NODE)

/* 从设备树获取配置 */
static const struct gpio_dt_spec bluetooth_led = GPIO_DT_SPEC_GET(BLUETOOTH_STATUS_NODE, gpios);

/* 私有数据结构 */
struct bluetooth_status_data {
    bool led_state;
    bool is_connected;
    bool timer_running;
    uint32_t last_activity_time;
    struct k_timer safety_timer;  /* 安全定时器，每10分钟检查一次 */
    struct k_timer blink_timer;   /* 闪烁定时器 */
};

static struct bluetooth_status_data bluetooth_data;

/* LED控制函数 */
static int set_led_state(bool state)
{
    int ret = gpio_pin_set_dt(&bluetooth_led, state ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to set LED state (err %d)", ret);
        return ret;
    }
    
    bluetooth_data.led_state = state;
    return 0;
}

/* 停止闪烁定时器 */
static void stop_blink_timer(void)
{
    if (bluetooth_data.timer_running) {
        k_timer_stop(&bluetooth_data.blink_timer);
        bluetooth_data.timer_running = false;
        LOG_DBG("Blink timer stopped");
    }
}

/* 启动闪烁定时器 */
static void start_blink_timer(void)
{
    if (!bluetooth_data.timer_running) {
        k_timer_start(&bluetooth_data.blink_timer, 
                     K_MSEC(500), K_MSEC(500));
        bluetooth_data.timer_running = true;
        LOG_DBG("Blink timer started");
    }
}

/* 闪烁定时器回调 */
static void blink_timer_handler(struct k_timer *timer)
{
    if (!bluetooth_data.is_connected) {
        set_led_state(!bluetooth_data.led_state);
    }
}

/* 安全定时器回调 - 每10分钟检查一次，防止事件丢失 */
static void safety_timer_handler(struct k_timer *timer)
{
    bool current_state = zmk_ble_active_profile_is_connected();
    
    /* 如果状态不一致，重新同步 */
    if (current_state != bluetooth_data.is_connected) {
        LOG_WRN("State mismatch detected! Fixing...");
        bluetooth_data.is_connected = current_state;
        
        if (current_state) {
            stop_blink_timer();
            set_led_state(false);
        } else {
            set_led_state(true);
            start_blink_timer();
        }
    }
    
    /* 记录活动时间 */
    bluetooth_data.last_activity_time = k_uptime_get_32();
}

/* 处理连接状态变化 */
static void handle_connection_change(bool connected)
{
    if (connected && !bluetooth_data.is_connected) {
        /* 连接建立 */
        LOG_INF("Bluetooth connected");
        stop_blink_timer();
        set_led_state(false);
        bluetooth_data.is_connected = true;
    } 
    else if (!connected && bluetooth_data.is_connected) {
        /* 连接断开 */
        LOG_INF("Bluetooth disconnected");
        bluetooth_data.is_connected = false;
        set_led_state(true);
        start_blink_timer();
    }
}

/* 事件监听函数 */
static int bluetooth_status_event_listener(const zmk_event_t *eh)
{
    struct zmk_ble_active_profile_changed *event = as_zmk_ble_active_profile_changed(eh);
    if (event) {
        bool connected = zmk_ble_active_profile_is_connected();
        handle_connection_change(connected);
        return 0;
    }
    return 0;
}

/* 初始化定时器 */
K_TIMER_DEFINE(bluetooth_data.blink_timer, blink_timer_handler, NULL);
K_TIMER_DEFINE(bluetooth_data.safety_timer, safety_timer_handler, NULL);

/* 初始化函数 */
static int bluetooth_status_init(void)
{
    LOG_INF("Initializing Bluetooth status indicator (interrupt mode)");
    
    /* 等待系统稳定 */
    k_sleep(K_MSEC(1000));
    
    /* 检查设备是否就绪 */
    if (!device_is_ready(bluetooth_led.port)) {
        LOG_ERR("Bluetooth status LED device not ready");
        return -ENODEV;
    }
    
    /* 配置GPIO引脚 */
    int ret = gpio_pin_configure_dt(&bluetooth_led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin (err %d)", ret);
        return ret;
    }
    
    /* 初始化为熄灭状态 */
    gpio_pin_set_dt(&bluetooth_led, 0);
    
    /* 初始化数据 */
    bluetooth_data.is_connected = zmk_ble_active_profile_is_connected();
    bluetooth_data.timer_running = false;
    bluetooth_data.led_state = false;
    bluetooth_data.last_activity_time = k_uptime_get_32();
    
    /* 根据初始状态设置 */
    if (bluetooth_data.is_connected) {
        LOG_INF("Initial state: Bluetooth connected, LED off");
        set_led_state(false);
    } else {
        LOG_INF("Initial state: Bluetooth disconnected, LED blinking");
        set_led_state(true);
        start_blink_timer();
    }
    
    /* 启动安全定时器（每10分钟检查一次） */
    k_timer_start(&bluetooth_data.safety_timer, 
                  K_MINUTES(10), K_MINUTES(10));
    
    LOG_INF("Bluetooth status indicator initialized with interrupt mode");
    return 0;
}

/* 订阅事件 */
ZMK_LISTENER(bluetooth_status, bluetooth_status_event_listener);
ZMK_SUBSCRIPTION(bluetooth_status, zmk_ble_active_profile_changed);

#else
/* 如果设备树节点不存在 */
static int bluetooth_status_init(void)
{
    LOG_WRN("Bluetooth status node not defined in device tree");
    return 0;
}
#endif /* DT_NODE_EXISTS(BLUETOOTH_STATUS_NODE) */

SYS_INIT(bluetooth_status_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);