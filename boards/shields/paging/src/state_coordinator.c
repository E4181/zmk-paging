#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(state_coordinator, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"
#include "state_coordinator.h"

// 状态协调器私有数据结构
struct state_coordinator_data {
    // 输入状态
    charging_state_t charging_state;
    bluetooth_state_t bluetooth_state;
    
    // 协调后状态
    system_led_state_t system_state;
    led_mode_t led_mode;
    uint32_t blink_interval_ms;
    
    // 回调
    system_state_changed_cb_t callback;
    struct k_work callback_work;
    
    // 控制标志
    bool initialized;
    bool charging_has_priority;
    int64_t last_state_change_time;
};

// 获取私有数据实例
static struct state_coordinator_data *get_data(void)
{
    static struct state_coordinator_data data = {
        .charging_state = CHARGING_STATE_ERROR,
        .bluetooth_state = BLUETOOTH_STATE_DISCONNECTED,
        .system_state = SYSTEM_LED_STATE_OFF,
        .led_mode = LED_MODE_OFF,
        .blink_interval_ms = CONFIG_BLUETOOTH_LED_BLINK_INTERVAL,
        .callback = NULL,
        .initialized = false,
        .charging_has_priority = CONFIG_CHARGING_HAS_PRIORITY,
        .last_state_change_time = 0,
    };
    return &data;
}

// 计算系统状态（根据优先级）
static void calculate_system_state(struct state_coordinator_data *data)
{
    system_led_state_t old_state = data->system_state;
    
    // 优先级逻辑：充电状态 > 蓝牙状态
    if (data->charging_has_priority) {
        switch (data->charging_state) {
        case CHARGING_STATE_CHARGING:
            data->system_state = SYSTEM_LED_STATE_CHARGING;
            data->led_mode = LED_MODE_ON;
            data->blink_interval_ms = 0;
            break;
            
        case CHARGING_STATE_FULL:
            data->system_state = SYSTEM_LED_STATE_FULL_CHARGE;
            data->led_mode = LED_MODE_OFF;
            data->blink_interval_ms = 0;
            break;
            
        case CHARGING_STATE_ERROR:
        default:
            // 充电状态错误，回退到蓝牙状态
            goto use_bluetooth_state;
        }
    } else {
        // 如果配置为蓝牙优先
        goto use_bluetooth_state;
    }
    
    return;
    
use_bluetooth_state:
    // 使用蓝牙状态
    switch (data->bluetooth_state) {
    case BLUETOOTH_STATE_CONNECTED:
        data->system_state = SYSTEM_LED_STATE_BT_CONNECTED;
        data->led_mode = LED_MODE_OFF;
        data->blink_interval_ms = 0;
        break;
        
    case BLUETOOTH_STATE_DISCONNECTED:
        data->system_state = SYSTEM_LED_STATE_BT_DISCONNECTED;
        data->led_mode = LED_MODE_BLINK_SLOW;
        data->blink_interval_ms = CONFIG_BLUETOOTH_LED_BLINK_INTERVAL;
        break;
        
    case BLUETOOTH_STATE_ERROR:
    default:
        data->system_state = SYSTEM_LED_STATE_ERROR;
        data->led_mode = LED_MODE_BLINK_FAST;
        data->blink_interval_ms = 250;
        break;
    }
}

// 异步回调工作函数
static void callback_work_handler(struct k_work *work)
{
    struct state_coordinator_data *data = CONTAINER_OF(work, struct state_coordinator_data, callback_work);
    
    if (data->callback) {
        data->callback(data->system_state, data->led_mode, data->blink_interval_ms);
    }
}

// 状态变化处理
static void on_state_changed(struct state_coordinator_data *data)
{
    system_led_state_t old_state = data->system_state;
    calculate_system_state(data);
    
    if (old_state != data->system_state) {
        data->last_state_change_time = k_uptime_get();
        
        LOG_INF("System LED state changed: %s -> %s (mode: %d, interval: %dms)",
               (old_state == SYSTEM_LED_STATE_CHARGING) ? "CHARGING" :
               (old_state == SYSTEM_LED_STATE_FULL_CHARGE) ? "FULL_CHARGE" :
               (old_state == SYSTEM_LED_STATE_BT_CONNECTED) ? "BT_CONNECTED" :
               (old_state == SYSTEM_LED_STATE_BT_DISCONNECTED) ? "BT_DISCONNECTED" : "UNKNOWN",
               
               (data->system_state == SYSTEM_LED_STATE_CHARGING) ? "CHARGING" :
               (data->system_state == SYSTEM_LED_STATE_FULL_CHARGE) ? "FULL_CHARGE" :
               (data->system_state == SYSTEM_LED_STATE_BT_CONNECTED) ? "BT_CONNECTED" :
               (data->system_state == SYSTEM_LED_STATE_BT_DISCONNECTED) ? "BT_DISCONNECTED" : "UNKNOWN",
               
               data->led_mode, data->blink_interval_ms);
        
        k_work_submit(&data->callback_work);
    }
}

// 初始化状态协调器
int state_coordinator_init(void)
{
    struct state_coordinator_data *data = get_data();
    
    if (data->initialized) {
        LOG_WRN("State coordinator already initialized");
        return 0;
    }
    
    LOG_INF("Initializing state coordinator");
    
    k_work_init(&data->callback_work, callback_work_handler);
    
    data->charging_state = CHARGING_STATE_ERROR;
    data->bluetooth_state = BLUETOOTH_STATE_DISCONNECTED;
    data->last_state_change_time = k_uptime_get();
    
    calculate_system_state(data);
    
    data->initialized = true;
    LOG_INF("State coordinator initialized. Initial state: %s",
           state_coordinator_get_state_str());
    
    return 0;
}

// 注册回调函数
int state_coordinator_register_callback(system_state_changed_cb_t callback)
{
    struct state_coordinator_data *data = get_data();
    
    if (!data->initialized) {
        LOG_ERR("State coordinator not initialized");
        return -ENODEV;
    }
    
    if (!callback) {
        LOG_ERR("Callback function is NULL");
        return -EINVAL;
    }
    
    data->callback = callback;
    
    LOG_DBG("System state callback registered");
    k_work_submit(&data->callback_work);
    
    return 0;
}

// 更新充电状态
void state_coordinator_update_charging(charging_state_t charging_state)
{
    struct state_coordinator_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    if (data->charging_state != charging_state) {
        LOG_DBG("Charging state updated: %d", charging_state);
        data->charging_state = charging_state;
        on_state_changed(data);
    }
}

// 更新蓝牙状态
void state_coordinator_update_bluetooth(bluetooth_state_t bluetooth_state)
{
    struct state_coordinator_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    if (data->bluetooth_state != bluetooth_state) {
        LOG_DBG("Bluetooth state updated: %d", bluetooth_state);
        data->bluetooth_state = bluetooth_state;
        on_state_changed(data);
    }
}

// 获取当前系统状态
system_led_state_t state_coordinator_get_current_state(void)
{
    struct state_coordinator_data *data = get_data();
    
    if (!data->initialized) {
        return SYSTEM_LED_STATE_ERROR;
    }
    
    return data->system_state;
}

// 获取系统状态字符串
const char* state_coordinator_get_state_str(void)
{
    system_led_state_t state = state_coordinator_get_current_state();
    
    switch (state) {
    case SYSTEM_LED_STATE_OFF:
        return "OFF";
    case SYSTEM_LED_STATE_CHARGING:
        return "CHARGING";
    case SYSTEM_LED_STATE_FULL_CHARGE:
        return "FULL_CHARGE";
    case SYSTEM_LED_STATE_BT_CONNECTED:
        return "BT_CONNECTED";
    case SYSTEM_LED_STATE_BT_DISCONNECTED:
        return "BT_DISCONNECTED";
    case SYSTEM_LED_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}