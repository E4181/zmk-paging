#pragma once

#include <zephyr/kernel.h>
#include "charging_monitor.h"
#include "bluetooth_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

// 系统最终LED状态
typedef enum {
    SYSTEM_LED_STATE_OFF = 0,
    SYSTEM_LED_STATE_CHARGING,
    SYSTEM_LED_STATE_FULL_CHARGE,
    SYSTEM_LED_STATE_BT_CONNECTED,
    SYSTEM_LED_STATE_BT_DISCONNECTED,
    SYSTEM_LED_STATE_ERROR
} system_led_state_t;

// LED控制模式
typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK_SLOW,
    LED_MODE_BLINK_FAST,
    LED_MODE_PULSE
} led_mode_t;

// 状态协调器回调函数类型
typedef void (*system_state_changed_cb_t)(system_led_state_t new_state, led_mode_t mode, uint32_t interval_ms);

// API接口
int state_coordinator_init(void);
int state_coordinator_register_callback(system_state_changed_cb_t callback);
system_led_state_t state_coordinator_get_current_state(void);
const char* state_coordinator_get_state_str(void);

// 手动更新状态
void state_coordinator_update_charging(charging_state_t charging_state);
void state_coordinator_update_bluetooth(bluetooth_state_t bluetooth_state);

#ifdef __cplusplus
}
#endif