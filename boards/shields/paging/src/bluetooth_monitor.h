#pragma once

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

// 蓝牙状态枚举
typedef enum {
    BLUETOOTH_STATE_DISCONNECTED = 0,
    BLUETOOTH_STATE_CONNECTED,
    BLUETOOTH_STATE_ERROR
} bluetooth_state_t;

// 蓝牙状态变化回调函数类型
typedef void (*bluetooth_state_changed_cb_t)(bluetooth_state_t new_state);

// API接口
int bluetooth_monitor_init(void);
int bluetooth_monitor_register_callback(bluetooth_state_changed_cb_t callback);
bluetooth_state_t bluetooth_monitor_get_state(void);
const char* bluetooth_monitor_get_state_str(void);

#ifdef __cplusplus
}
#endif