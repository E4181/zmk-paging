#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bluetooth_monitor, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"
#include "bluetooth_monitor.h"

// 检查间隔（毫秒）
#define BLUETOOTH_CHECK_INTERVAL_MS 3000

// 蓝牙监控器私有数据结构
struct bluetooth_monitor_data {
    bluetooth_state_t current_state;
    struct k_work_delayable status_check_work;
    struct k_work callback_work;
    bluetooth_state_changed_cb_t callback;
    bool initialized;
    int64_t last_state_change_time;
};

// 获取私有数据实例
static struct bluetooth_monitor_data *get_data(void)
{
    static struct bluetooth_monitor_data data = {
        .current_state = BLUETOOTH_STATE_DISCONNECTED,
        .callback = NULL,
        .initialized = false,
        .last_state_change_time = 0,
    };
    return &data;
}

// 检查蓝牙连接状态 - 简化为始终返回未连接，用于测试LED闪烁
static bluetooth_state_t check_bluetooth_connection(void)
{
    // 简化方案：始终返回未连接，这样LED会闪烁
    // 在实际使用中，您需要根据实际需求实现蓝牙状态检测
    return BLUETOOTH_STATE_DISCONNECTED;
}

// 异步回调工作函数
static void callback_work_handler(struct k_work *work)
{
    struct bluetooth_monitor_data *data = CONTAINER_OF(work, struct bluetooth_monitor_data, callback_work);
    
    if (data->callback) {
        data->callback(data->current_state);
    }
}

// 状态检查工作函数
static void status_check_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct bluetooth_monitor_data *data = CONTAINER_OF(dwork, struct bluetooth_monitor_data, status_check_work);
    
    if (!data->initialized) {
        return;
    }
    
    bluetooth_state_t new_state = check_bluetooth_connection();
    
    if (new_state != data->current_state) {
        data->current_state = new_state;
        data->last_state_change_time = k_uptime_get();
        
        LOG_INF("Bluetooth state changed to: %s", bluetooth_monitor_get_state_str());
        
        k_work_submit(&data->callback_work);
    }
    
    // 继续下一次检查
    k_work_reschedule(dwork, K_MSEC(BLUETOOTH_CHECK_INTERVAL_MS));
}

// 初始化蓝牙监控器
int bluetooth_monitor_init(void)
{
    struct bluetooth_monitor_data *data = get_data();
    
    if (data->initialized) {
        LOG_WRN("Bluetooth monitor already initialized");
        return 0;
    }
    
    LOG_INF("Initializing bluetooth monitor (simplified version)");
    
    k_work_init_delayable(&data->status_check_work, status_check_work_handler);
    k_work_init(&data->callback_work, callback_work_handler);
    
    // 初始化状态为未连接
    data->current_state = BLUETOOTH_STATE_DISCONNECTED;
    data->last_state_change_time = k_uptime_get();
    
    LOG_INF("Initial bluetooth state: %s", bluetooth_monitor_get_state_str());
    
    // 启动状态检查
    k_work_reschedule(&data->status_check_work, K_MSEC(BLUETOOTH_CHECK_INTERVAL_MS));
    
    data->initialized = true;
    LOG_INF("Bluetooth monitor initialized successfully");
    
    return 0;
}

// 注册回调函数
int bluetooth_monitor_register_callback(bluetooth_state_changed_cb_t callback)
{
    struct bluetooth_monitor_data *data = get_data();
    
    if (!data->initialized) {
        LOG_ERR("Bluetooth monitor not initialized");
        return -ENODEV;
    }
    
    if (!callback) {
        LOG_ERR("Callback function is NULL");
        return -EINVAL;
    }
    
    data->callback = callback;
    
    LOG_DBG("Bluetooth callback registered");
    
    // 立即触发一次回调
    k_work_submit(&data->callback_work);
    
    return 0;
}

// 获取当前蓝牙状态
bluetooth_state_t bluetooth_monitor_get_state(void)
{
    struct bluetooth_monitor_data *data = get_data();
    
    if (!data->initialized) {
        return BLUETOOTH_STATE_ERROR;
    }
    
    return data->current_state;
}

// 获取蓝牙状态字符串
const char* bluetooth_monitor_get_state_str(void)
{
    bluetooth_state_t state = bluetooth_monitor_get_state();
    
    switch (state) {
    case BLUETOOTH_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case BLUETOOTH_STATE_CONNECTED:
        return "CONNECTED";
    case BLUETOOTH_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}