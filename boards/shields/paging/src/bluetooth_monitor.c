#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

LOG_MODULE_REGISTER(bluetooth_monitor, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"
#include "bluetooth_monitor.h"

// 检查间隔（毫秒）
#define BLUETOOTH_CHECK_INTERVAL_MS 2000

// 蓝牙监控器私有数据结构
struct bluetooth_monitor_data {
    bluetooth_state_t current_state;
    struct k_work_delayable status_check_work;
    struct k_work callback_work;
    bluetooth_state_changed_cb_t callback;
    bool initialized;
    int64_t last_state_change_time;
    uint32_t connection_count;
};

// 获取私有数据实例
static struct bluetooth_monitor_data *get_data(void)
{
    static struct bluetooth_monitor_data data = {
        .current_state = BLUETOOTH_STATE_DISCONNECTED,
        .callback = NULL,
        .initialized = false,
        .last_state_change_time = 0,
        .connection_count = 0,
    };
    return &data;
}

// 蓝牙连接状态回调函数
static void bt_connected(struct bt_conn *conn, uint8_t err)
{
    struct bluetooth_monitor_data *data = get_data();
    
    if (err) {
        LOG_WRN("Connection failed (err %u)", err);
        return;
    }
    
    data->connection_count++;
    LOG_INF("Bluetooth connected (count: %u)", data->connection_count);
}

static void bt_disconnected(struct bt_conn *conn, uint8_t reason)
{
    struct bluetooth_monitor_data *data = get_data();
    
    LOG_INF("Bluetooth disconnected (reason %u)", reason);
    
    if (data->connection_count > 0) {
        data->connection_count--;
    }
}

// 蓝牙连接回调结构
BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = bt_connected,
    .disconnected = bt_disconnected,
};

// 检查蓝牙连接状态
static bluetooth_state_t check_bluetooth_connection(void)
{
    struct bluetooth_monitor_data *data = get_data();
    
    // 方法1：检查活跃连接数
    if (data->connection_count > 0) {
        return BLUETOOTH_STATE_CONNECTED;
    }
    
    // 方法2：使用蓝牙API检查
    #ifdef CONFIG_BT_CENTRAL
    // 作为中心设备检查连接
    struct bt_conn *conn = bt_conn_lookup_state_le(BT_ID_DEFAULT, NULL, BT_CONN_STATE_CONNECTED);
    if (conn) {
        bt_conn_unref(conn);
        return BLUETOOTH_STATE_CONNECTED;
    }
    #endif
    
    #ifdef CONFIG_BT_PERIPHERAL
    // 作为外设检查广播状态
    if (bt_le_is_advertising()) {
        return BLUETOOTH_STATE_DISCONNECTED; // 正在广播，但未连接
    }
    #endif
    
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
        const char *old_state_str = bluetooth_monitor_get_state_str();
        const char *new_state_str = (new_state == BLUETOOTH_STATE_CONNECTED) ? "CONNECTED" : 
                                   (new_state == BLUETOOTH_STATE_DISCONNECTED) ? "DISCONNECTED" : "ERROR";
        
        data->current_state = new_state;
        data->last_state_change_time = k_uptime_get();
        
        LOG_INF("Bluetooth state changed: %s -> %s (connections: %u)", 
                old_state_str, new_state_str, data->connection_count);
        
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
    
    LOG_INF("Initializing bluetooth monitor");
    
    k_work_init_delayable(&data->status_check_work, status_check_work_handler);
    k_work_init(&data->callback_work, callback_work_handler);
    
    // 初始化状态为未连接
    data->current_state = BLUETOOTH_STATE_DISCONNECTED;
    data->last_state_change_time = k_uptime_get();
    data->connection_count = 0;
    
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