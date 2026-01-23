/*
 * Copyright (c) 2025 Your Name
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 配置LED引脚 */
#define BLUETOOTH_STATUS_LED_NODE DT_ALIAS(bluetooth_status_led)
#define BLUETOOTH_STATUS_LED_PIN 26  /* P0.26 */

#if DT_NODE_HAS_STATUS(BLUETOOTH_STATUS_LED_NODE, okay)
static const struct gpio_dt_spec bluetooth_led = 
    GPIO_DT_SPEC_GET(BLUETOOTH_STATUS_LED_NODE, gpios);
#else
#error "Bluetooth status LED not defined in device tree"
#endif

/* 闪烁模式定义 */
enum bluetooth_status_pattern {
    STATUS_PATTERN_CONNECTED = 0,      /* 连接成功 - 常亮 */
    STATUS_PATTERN_DISCONNECTED,       /* 未连接 - 快速闪烁 */
    STATUS_PATTERN_ADVERTISING,        /* 广播中 - 慢速闪烁 */
    STATUS_PATTERN_CONNECTING,         /* 连接中 - 双闪 */
};

/* 设备私有数据结构 */
struct bluetooth_status_data {
    enum bluetooth_status_pattern current_pattern;
    bool led_state;
    uint8_t pattern_step;
    struct k_work_delayable led_work;
    struct k_work status_check_work;
    bool ble_initialized;
    bool advertising_active;
    uint32_t connection_check_timestamp;
};

/* 设备配置结构 */
struct bluetooth_status_config {
    /* 可以添加配置参数，如闪烁频率等 */
};

/* 全局实例 */
static struct bluetooth_status_data bluetooth_data;

/* 闪烁定时参数（单位：毫秒） */
#define BLINK_INTERVAL_FAST     200  /* 快速闪烁间隔 */
#define BLINK_INTERVAL_SLOW     500  /* 慢速闪烁间隔 */
#define BLINK_INTERVAL_DOUBLE   100  /* 双闪间隔 */
#define DOUBLE_BLINK_PAUSE      300  /* 双闪之间的暂停 */
#define STATUS_CHECK_INTERVAL   1000 /* 状态检查间隔（毫秒） */

/* 闪烁模式定义表 */
struct blink_pattern {
    uint32_t on_time;
    uint32_t off_time;
    uint8_t repeat_count;
};

static const struct blink_pattern patterns[] = {
    [STATUS_PATTERN_CONNECTED] = {
        .on_time = 0,           /* 常亮 - 不闪烁 */
        .off_time = 0,
        .repeat_count = 0,
    },
    [STATUS_PATTERN_DISCONNECTED] = {
        .on_time = BLINK_INTERVAL_FAST,
        .off_time = BLINK_INTERVAL_FAST,
        .repeat_count = 0,      /* 无限重复 */
    },
    [STATUS_PATTERN_ADVERTISING] = {
        .on_time = BLINK_INTERVAL_SLOW,
        .off_time = BLINK_INTERVAL_SLOW,
        .repeat_count = 0,      /* 无限重复 */
    },
    [STATUS_PATTERN_CONNECTING] = {
        .on_time = BLINK_INTERVAL_DOUBLE,
        .off_time = BLINK_INTERVAL_DOUBLE,
        .repeat_count = 2,      /* 双闪：闪两次 */
    },
};

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

/* 获取当前蓝牙连接状态 */
static bool check_bluetooth_connected(void)
{
    if (!bluetooth_data.ble_initialized) {
        return false;
    }
    
    return zmk_ble_active_profile_is_connected();
}

/* 判断是否在广播状态 */
static bool check_advertising_status(void)
{
    /* 这里需要根据实际需求判断是否在广播状态 */
    /* 可以根据是否有活跃连接和配置文件状态来判断 */
    if (!bluetooth_data.ble_initialized) {
        return false;
    }
    
    /* 如果没有连接且活跃配置文件是开放的，则可能在广播 */
    if (!zmk_ble_active_profile_is_connected() && 
        zmk_ble_active_profile_is_open()) {
        return true;
    }
    
    return false;
}

/* 更新LED闪烁模式 */
static void update_led_pattern(void)
{
    enum bluetooth_status_pattern new_pattern;
    
    /* 检查蓝牙连接状态 */
    if (check_bluetooth_connected()) {
        new_pattern = STATUS_PATTERN_CONNECTED;
    } 
    /* 检查是否在广播状态 */
    else if (check_advertising_status()) {
        new_pattern = STATUS_PATTERN_ADVERTISING;
    }
    /* 其他情况视为未连接 */
    else {
        new_pattern = STATUS_PATTERN_DISCONNECTED;
    }
    
    /* 如果模式改变，重置闪烁步骤 */
    if (new_pattern != bluetooth_data.current_pattern) {
        bluetooth_data.current_pattern = new_pattern;
        bluetooth_data.pattern_step = 0;
        LOG_DBG("Bluetooth status pattern changed to %d", new_pattern);
    }
}

/* LED闪烁工作函数 */
static void led_blink_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct bluetooth_status_data *data = CONTAINER_OF(dwork, struct bluetooth_status_data, led_work);
    const struct blink_pattern *pattern = &patterns[data->current_pattern];
    
    /* 更新当前蓝牙状态 */
    update_led_pattern();
    
    /* 处理不同闪烁模式 */
    switch (data->current_pattern) {
        case STATUS_PATTERN_CONNECTED:
            /* 连接成功 - 常亮 */
            set_led_state(true);
            /* 不需要定时器，直接返回 */
            return;
            
        case STATUS_PATTERN_DISCONNECTED:
        case STATUS_PATTERN_ADVERTISING:
            /* 快速/慢速闪烁 */
            set_led_state(!data->led_state);
            k_work_reschedule(dwork, K_MSEC(data->led_state ? pattern->on_time : pattern->off_time));
            break;
            
        case STATUS_PATTERN_CONNECTING:
            /* 双闪模式 */
            if (data->pattern_step < pattern->repeat_count) {
                set_led_state(!data->led_state);
                data->pattern_step++;
                k_work_reschedule(dwork, K_MSEC(pattern->on_time));
            } else {
                /* 完成一次双闪循环，暂停后重新开始 */
                set_led_state(false);
                data->pattern_step = 0;
                k_work_reschedule(dwork, K_MSEC(DOUBLE_BLINK_PAUSE));
            }
            break;
    }
}

/* 状态检查工作函数 */
static void status_check_work_handler(struct k_work *work)
{
    struct bluetooth_status_data *data = CONTAINER_OF(work, struct bluetooth_status_data, status_check_work);
    
    /* 检查蓝牙状态并更新LED模式 */
    update_led_pattern();
    
    /* 如果LED定时器未运行，启动它（仅在需要闪烁时） */
    if (data->current_pattern != STATUS_PATTERN_CONNECTED) {
        if (!k_work_delayable_is_pending(&data->led_work)) {
            k_work_schedule(&data->led_work, K_MSEC(0));
        }
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
    
    /* 处理活动状态变更事件 */
    struct zmk_activity_state_changed *activity_event = as_zmk_activity_state_changed(eh);
    if (activity_event != NULL) {
        LOG_DBG("Activity state changed: %d", activity_event->state);
        /* 可以根据活动状态调整LED行为，例如休眠时降低亮度或停止闪烁 */
        /* 这里可以根据需要实现 */
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
    bluetooth_data.current_pattern = STATUS_PATTERN_DISCONNECTED;
    bluetooth_data.led_state = false;
    bluetooth_data.pattern_step = 0;
    bluetooth_data.ble_initialized = false;
    bluetooth_data.advertising_active = false;
    bluetooth_data.connection_check_timestamp = k_uptime_get_32();
    
    /* 配置GPIO引脚 */
    if (!device_is_ready(bluetooth_led.port)) {
        LOG_ERR("LED device not ready");
        return -ENODEV;
    }
    
    ret = gpio_pin_configure_dt(&bluetooth_led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin (err %d)", ret);
        return ret;
    }
    
    /* 初始化工作项 */
    k_work_init_delayable(&bluetooth_data.led_work, led_blink_work_handler);
    k_work_init(&bluetooth_data.status_check_work, status_check_work_handler);
    
    /* 标记蓝牙为已初始化（这里假设蓝牙在驱动初始化后会被初始化） */
    /* 在实际使用中，可能需要等待蓝牙初始化完成的事件 */
    bluetooth_data.ble_initialized = true;
    
    /* 初始状态检查 */
    update_led_pattern();
    
    /* 如果处于非连接状态，启动闪烁 */
    if (bluetooth_data.current_pattern != STATUS_PATTERN_CONNECTED) {
        k_work_schedule(&bluetooth_data.led_work, K_MSEC(0));
    } else {
        /* 连接状态，LED常亮 */
        set_led_state(true);
    }
    
    LOG_INF("Bluetooth status indicator initialized");
    return 0;
}

/* 订阅事件 */
ZMK_LISTENER(bluetooth_status, bluetooth_status_event_listener);
ZMK_SUBSCRIPTION(bluetooth_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(bluetooth_status, zmk_activity_state_changed);

/* 设备定义 */
DEVICE_DEFINE(bluetooth_status, "bluetooth_status", 
              bluetooth_status_init, NULL, NULL, NULL,
              POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);