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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 设备树节点 */
#define BLUETOOTH_STATUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(bluetooth_status)

#if DT_NODE_HAS_STATUS(BLUETOOTH_STATUS_NODE, okay)

/* 从设备树获取配置 */
#define BLUETOOTH_STATUS_LABEL        DT_LABEL(BLUETOOTH_STATUS_NODE)
#define BLUETOOTH_STATUS_CHECK_MS     DT_PROP(BLUETOOTH_STATUS_NODE, bluetooth_connection_interval_ms)
#define BLUETOOTH_STATUS_BLINK_MS     DT_PROP(BLUETOOTH_STATUS_NODE, led_blink_interval_ms)

/* 设备私有数据结构 */
struct bluetooth_status_data {
    const struct gpio_dt_spec led;
    bool led_state;
    bool is_connected;
    uint64_t last_blink_time;
    struct k_timer status_timer;
};

/* 全局设备实例 */
static struct bluetooth_status_data bluetooth_data = {
    .led = GPIO_DT_SPEC_GET(BLUETOOTH_STATUS_NODE, gpios),
};

/* LED控制函数 */
static int set_led_state(bool state)
{
    int ret;
    
    if (!device_is_ready(bluetooth_data.led.port)) {
        return -ENODEV;
    }
    
    ret = gpio_pin_set_dt(&bluetooth_data.led, state ? 1 : 0);
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
            LOG_DBG("Bluetooth connected, LED off");
        } else {
            /* 断开连接，开始闪烁 */
            set_led_state(true);  /* 先点亮 */
            bluetooth_data.last_blink_time = current_time;
            LOG_DBG("Bluetooth disconnected, LED blinking");
        }
    }
    
    /* 如果未连接，处理闪烁 */
    if (!bluetooth_data.is_connected) {
        if (current_time - bluetooth_data.last_blink_time >= BLUETOOTH_STATUS_BLINK_MS) {
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

/* 初始化函数 */
static int bluetooth_status_init(void)
{
    int ret;
    
    LOG_INF("Initializing Bluetooth status indicator");
    
    /* 检查设备是否就绪 */
    if (!device_is_ready(bluetooth_data.led.port)) {
        LOG_ERR("Bluetooth status LED device not ready");
        return -ENODEV;
    }
    
    /* 配置GPIO引脚 */
    ret = gpio_pin_configure_dt(&bluetooth_data.led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin (err %d)", ret);
        return ret;
    }
    
    /* 初始化为熄灭状态 */
    ret = gpio_pin_set_dt(&bluetooth_data.led, 0);
    if (ret < 0) {
        LOG_ERR("Failed to set initial LED state (err %d)", ret);
        return ret;
    }
    
    /* 检查初始连接状态 */
    bluetooth_data.is_connected = zmk_ble_active_profile_is_connected();
    bluetooth_data.last_blink_time = k_uptime_get();
    bluetooth_data.led_state = false;
    
    /* 根据初始状态设置LED */
    if (bluetooth_data.is_connected) {
        LOG_INF("Initial state: Bluetooth connected, LED off");
    } else {
        /* 未连接，点亮LED */
        ret = set_led_state(true);
        if (ret < 0) {
            LOG_ERR("Failed to set initial LED state (err %d)", ret);
            return ret;
        }
        LOG_INF("Initial state: Bluetooth disconnected, LED blinking");
    }
    
    /* 初始化并启动定时器 */
    k_timer_init(&bluetooth_data.status_timer, bluetooth_status_timer_handler, NULL);
    k_timer_start(&bluetooth_data.status_timer, 
                  K_MSEC(BLUETOOTH_STATUS_CHECK_MS), 
                  K_MSEC(BLUETOOTH_STATUS_CHECK_MS));
    
    LOG_INF("Bluetooth status indicator initialized (check interval: %d ms, blink interval: %d ms)",
            BLUETOOTH_STATUS_CHECK_MS, BLUETOOTH_STATUS_BLINK_MS);
    return 0;
}

#else
/* 如果设备树节点不存在 */
static int bluetooth_status_init(void)
{
    LOG_WRN("Bluetooth status node not defined in device tree");
    return 0;
}
#endif /* DT_NODE_HAS_STATUS(BLUETOOTH_STATUS_NODE, okay) */

/* 使用正确的SYS_INIT签名（无参数） */
SYS_INIT(bluetooth_status_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);