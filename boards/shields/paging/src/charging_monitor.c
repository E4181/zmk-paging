#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(charging_monitor, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"
#include "charging_monitor.h"

// 使用配置值
#define CHARGING_GPIO_PORT      DT_NODELABEL(gpio1)
#define CHARGING_GPIO_PIN       CONFIG_CHARGING_GPIO_PIN
#define CHARGING_GPIO_FLAGS     (GPIO_ACTIVE_LOW | GPIO_PULL_UP)

// 轮询间隔配置（毫秒）
#define POLL_INTERVAL_CHARGING_MS   CONFIG_CHARGING_POLL_INTERVAL_CHARGING_MS
#define POLL_INTERVAL_FULL_MS       CONFIG_CHARGING_POLL_INTERVAL_FULL_MS
#define POLL_INTERVAL_ERROR_MS      30000
#define POLL_INTERVAL_INTERRUPT_MS  30000

// 空闲检测配置
#define IDLE_TIMEOUT_MS           30000
#define IDLE_MULTIPLIER           2

// 最大连续错误次数
#define MAX_CONSECUTIVE_ERRORS    5

// 防抖时间（毫秒）
#define DEBOUNCE_TIME_MS          1000

// 中断防抖时间（微秒）
#define INTERRUPT_DEBOUNCE_US     50000

// 工作模式枚举
enum work_mode {
    MODE_POLLING = 0,
    MODE_INTERRUPT,
    MODE_ERROR
};

// 充电监控器私有数据结构（带中断支持）
struct charging_monitor_data {
    // 状态变量
    charging_state_t current_state;
    
    // 工作队列
    struct k_work_delayable status_check_work;
    struct k_work callback_work;
    struct k_work interrupt_work;
    
    // GPIO相关
    const struct device *gpio_dev;
    struct gpio_callback gpio_cb;
    
    // 回调函数
    charging_state_changed_cb_t callback;
    
    // 统计和控制标志
    uint32_t consecutive_errors;
    uint32_t interrupt_count;
    int64_t last_activity_time;
    int64_t last_state_change_time;
    int64_t last_interrupt_time;
    bool initialized : 1;
    bool polling_active : 1;
    bool system_idle : 1;
    bool interrupt_enabled : 1;
    bool in_interrupt : 1;
    enum work_mode mode;
};

// 获取私有数据实例
static struct charging_monitor_data *get_data(void)
{
    static struct charging_monitor_data data = {
        .current_state = CHARGING_STATE_ERROR,
        .callback = NULL,
        .gpio_dev = NULL,
        .consecutive_errors = 0,
        .interrupt_count = 0,
        .last_activity_time = 0,
        .last_state_change_time = 0,
        .last_interrupt_time = 0,
        .initialized = false,
        .polling_active = true,
        .system_idle = false,
        .interrupt_enabled = false,
        .in_interrupt = false,
        .mode = MODE_POLLING,
    };
    return &data;
}

// GPIO中断处理函数
static void gpio_interrupt_handler(const struct device *dev, 
                                   struct gpio_callback *cb, 
                                   uint32_t pins)
{
    struct charging_monitor_data *data = CONTAINER_OF(cb, struct charging_monitor_data, gpio_cb);
    int64_t now = k_uptime_get();
    
    // 中断防抖
    if ((now - data->last_interrupt_time) * 1000 < INTERRUPT_DEBOUNCE_US) {
        LOG_DBG("Interrupt debounced, too frequent");
        return;
    }
    
    data->last_interrupt_time = now;
    data->interrupt_count++;
    data->last_activity_time = now;
    data->in_interrupt = true;
    
    k_work_submit(&data->interrupt_work);
    
    LOG_DBG("GPIO interrupt detected, count: %u", data->interrupt_count);
}

// 中断工作处理函数
static void interrupt_work_handler(struct k_work *work)
{
    struct charging_monitor_data *data = CONTAINER_OF(work, struct charging_monitor_data, interrupt_work);
    
    if (!data->initialized || !data->gpio_dev) {
        return;
    }
    
    LOG_DBG("Processing interrupt work");
    
    k_work_cancel_delayable(&data->status_check_work);
    k_work_reschedule(&data->status_check_work, K_NO_WAIT);
    data->in_interrupt = false;
}

// 异步回调工作函数
static void callback_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    struct charging_monitor_data *data = get_data();
    
    if (data->callback) {
        data->callback(data->current_state);
    }
}

// 状态变化防抖检查
static bool should_process_state_change(struct charging_monitor_data *data, 
                                       charging_state_t new_state)
{
    int64_t now = k_uptime_get();
    
    if (new_state == CHARGING_STATE_ERROR || 
        data->current_state == CHARGING_STATE_ERROR) {
        return true;
    }
    
    if (data->in_interrupt) {
        if (now - data->last_state_change_time < DEBOUNCE_TIME_MS / 2) {
            LOG_DBG("Interrupt-triggered state change debounced");
            return false;
        }
        return true;
    }
    
    if (now - data->last_state_change_time < DEBOUNCE_TIME_MS) {
        LOG_DBG("Polling state change debounced: %d -> %d", 
                data->current_state, new_state);
        return false;
    }
    
    return true;
}

// 智能轮询间隔计算
static uint32_t calculate_polling_interval(struct charging_monitor_data *data, 
                                          charging_state_t state, bool system_idle)
{
    uint32_t base_interval;
    
    switch (data->mode) {
    case MODE_INTERRUPT:
        base_interval = POLL_INTERVAL_INTERRUPT_MS;
        break;
    case MODE_POLLING:
    case MODE_ERROR:
    default:
        switch (state) {
        case CHARGING_STATE_CHARGING:
            base_interval = POLL_INTERVAL_CHARGING_MS;
            break;
        case CHARGING_STATE_FULL:
            base_interval = POLL_INTERVAL_FULL_MS;
            break;
        case CHARGING_STATE_ERROR:
            base_interval = POLL_INTERVAL_ERROR_MS * (1 + (data->consecutive_errors / 2));
            if (base_interval > 120000) base_interval = 120000;
            break;
        default:
            base_interval = POLL_INTERVAL_FULL_MS;
        }
        break;
    }
    
    if (system_idle && state != CHARGING_STATE_CHARGING) {
        base_interval *= IDLE_MULTIPLIER;
    }
    
    return base_interval;
}

// 记录活动时间
static void record_activity(void)
{
    struct charging_monitor_data *data = get_data();
    data->last_activity_time = k_uptime_get();
    
    if (data->system_idle) {
        data->system_idle = false;
        LOG_DBG("Activity detected, exiting idle mode");
    }
}

// 检查系统是否空闲
static bool check_system_idle(void)
{
    struct charging_monitor_data *data = get_data();
    int64_t now = k_uptime_get();
    bool is_idle = ((now - data->last_activity_time) > IDLE_TIMEOUT_MS);
    
    if (is_idle != data->system_idle) {
        data->system_idle = is_idle;
        LOG_DBG("System %s", is_idle ? "idle" : "active");
    }
    
    return is_idle;
}

// 尝试启用中断模式
static bool try_enable_interrupt(struct charging_monitor_data *data)
{
    int ret;
    
    ret = gpio_pin_interrupt_configure(data->gpio_dev, CHARGING_GPIO_PIN,
                                      GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_WRN("Failed to configure GPIO interrupt: %d (falling back to polling)", ret);
        return false;
    }
    
    gpio_init_callback(&data->gpio_cb, gpio_interrupt_handler, BIT(CHARGING_GPIO_PIN));
    
    ret = gpio_add_callback(data->gpio_dev, &data->gpio_cb);
    if (ret < 0) {
        LOG_WRN("Failed to add GPIO callback: %d (falling back to polling)", ret);
        gpio_pin_interrupt_configure(data->gpio_dev, CHARGING_GPIO_PIN, GPIO_INT_DISABLE);
        return false;
    }
    
    data->interrupt_enabled = true;
    data->mode = MODE_INTERRUPT;
    LOG_INF("GPIO interrupt enabled for CHRG pin");
    
    return true;
}

// 状态检查工作函数
static void status_check_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct charging_monitor_data *data = CONTAINER_OF(dwork, struct charging_monitor_data, status_check_work);
    
    if (!data->initialized || !data->gpio_dev) {
        LOG_WRN("Charging monitor not initialized");
        k_work_reschedule(dwork, K_MSEC(POLL_INTERVAL_ERROR_MS));
        return;
    }
    
    if (!data->polling_active) {
        LOG_DBG("Polling paused");
        return;
    }
    
    bool system_idle = check_system_idle();
    int pin_state = gpio_pin_get(data->gpio_dev, CHARGING_GPIO_PIN);
    
    if (pin_state < 0) {
        LOG_ERR("Failed to read CHRG pin: %d", pin_state);
        
        if (data->mode == MODE_INTERRUPT) {
            LOG_WRN("Interrupt mode error, falling back to polling");
            data->interrupt_enabled = false;
            data->mode = MODE_POLLING;
            gpio_pin_interrupt_configure(data->gpio_dev, CHARGING_GPIO_PIN, GPIO_INT_DISABLE);
        }
        
        if (data->consecutive_errors < MAX_CONSECUTIVE_ERRORS) {
            data->consecutive_errors++;
        }
        
        data->current_state = CHARGING_STATE_ERROR;
        uint32_t interval = calculate_polling_interval(data, CHARGING_STATE_ERROR, system_idle);
        k_work_reschedule(dwork, K_MSEC(interval));
        return;
    }
    
    data->consecutive_errors = 0;
    charging_state_t new_state = (pin_state == 1) ? CHARGING_STATE_CHARGING : CHARGING_STATE_FULL;
    charging_state_t current_state = data->current_state;
    
    if (new_state != current_state) {
        if (should_process_state_change(data, new_state)) {
            const char *old_state_str = (current_state == CHARGING_STATE_CHARGING) ? "CHARGING" : 
                                       (current_state == CHARGING_STATE_FULL) ? "FULL" : "ERROR";
            const char *new_state_str = (new_state == CHARGING_STATE_CHARGING) ? "CHARGING" : "FULL";
            const char *trigger_str = data->in_interrupt ? "interrupt" : "polling";
            
            LOG_INF("Charging state changed (%s): %s -> %s", 
                    trigger_str, old_state_str, new_state_str);
            
            data->current_state = new_state;
            data->last_state_change_time = k_uptime_get();
            
            k_work_submit(&data->callback_work);
        } else {
            LOG_DBG("State change filtered by debounce: %d -> %d", 
                    current_state, new_state);
        }
    }
    
    uint32_t interval = calculate_polling_interval(data, new_state, system_idle);
    k_work_reschedule(dwork, K_MSEC(interval));
}

// 初始化充电监控器
int charging_monitor_init(void)
{
    struct charging_monitor_data *data = get_data();
    int ret;
    
    if (data->initialized) {
        LOG_WRN("Charging monitor already initialized");
        return 0;
    }
    
    LOG_DBG("Initializing charging monitor with interrupt support");
    
    data->gpio_dev = DEVICE_DT_GET(CHARGING_GPIO_PORT);
    if (!data->gpio_dev) {
        LOG_ERR("Failed to get GPIO1 device");
        return -ENODEV;
    }
    
    if (!device_is_ready(data->gpio_dev)) {
        LOG_ERR("GPIO1 device not ready");
        return -ENODEV;
    }
    
    ret = gpio_pin_configure(data->gpio_dev, CHARGING_GPIO_PIN, GPIO_INPUT | CHARGING_GPIO_FLAGS);
    if (ret < 0) {
        LOG_ERR("Failed to configure CHRG GPIO: %d", ret);
        return ret;
    }
    
    LOG_INF("Charging monitor configured: GPIO1 pin %d (P1.09), flags: 0x%x", 
            CHARGING_GPIO_PIN, CHARGING_GPIO_FLAGS);
    
    k_work_init_delayable(&data->status_check_work, status_check_work_handler);
    k_work_init(&data->callback_work, callback_work_handler);
    k_work_init(&data->interrupt_work, interrupt_work_handler);
    
    bool interrupt_success = try_enable_interrupt(data);
    
    if (interrupt_success) {
        LOG_INF("Charging monitor operating in interrupt mode");
    } else {
        LOG_INF("Charging monitor operating in polling mode");
        data->mode = MODE_POLLING;
    }
    
    record_activity();
    
    int initial_state = gpio_pin_get(data->gpio_dev, CHARGING_GPIO_PIN);
    if (initial_state >= 0) {
        data->current_state = (initial_state == 1) ? CHARGING_STATE_CHARGING : CHARGING_STATE_FULL;
        data->last_state_change_time = k_uptime_get();
        
        LOG_INF("Initial charging state: %s (mode: %s)", 
                (data->current_state == CHARGING_STATE_CHARGING) ? "CHARGING" : "FULL",
                data->interrupt_enabled ? "interrupt" : "polling");
    } else {
        LOG_ERR("Failed to read initial CHRG pin state: %d", initial_state);
        data->current_state = CHARGING_STATE_ERROR;
        data->mode = MODE_ERROR;
    }
    
    uint32_t initial_interval;
    if (data->mode == MODE_INTERRUPT) {
        initial_interval = POLL_INTERVAL_INTERRUPT_MS;
    } else {
        initial_interval = calculate_polling_interval(data, data->current_state, false);
    }
    
    k_work_reschedule(&data->status_check_work, K_MSEC(initial_interval));
    
    data->initialized = true;
    LOG_INF("Charging monitor initialized successfully");
    
    return 0;
}

// 注册回调函数
int charging_monitor_register_callback(charging_state_changed_cb_t callback)
{
    struct charging_monitor_data *data = get_data();
    
    if (!data->initialized) {
        LOG_ERR("Charging monitor not initialized");
        return -ENODEV;
    }
    
    if (!callback) {
        LOG_ERR("Callback function is NULL");
        return -EINVAL;
    }
    
    data->callback = callback;
    
    LOG_DBG("Callback registered");
    k_work_submit(&data->callback_work);
    
    return 0;
}

// 获取当前充电状态
charging_state_t charging_monitor_get_state(void)
{
    struct charging_monitor_data *data = get_data();
    
    if (!data->initialized) {
        return CHARGING_STATE_ERROR;
    }
    
    return data->current_state;
}

// 获取充电状态字符串
const char* charging_monitor_get_state_str(void)
{
    charging_state_t state = charging_monitor_get_state();
    
    switch (state) {
    case CHARGING_STATE_CHARGING:
        return "CHARGING";
    case CHARGING_STATE_FULL:
        return "FULL";
    case CHARGING_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

// 获取当前工作模式
const char* charging_monitor_get_mode_str(void)
{
    struct charging_monitor_data *data = get_data();
    
    if (!data->initialized) {
        return "UNINITIALIZED";
    }
    
    switch (data->mode) {
    case MODE_POLLING:
        return "POLLING";
    case MODE_INTERRUPT:
        return "INTERRUPT";
    case MODE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

// 获取中断统计
uint32_t charging_monitor_get_interrupt_count(void)
{
    struct charging_monitor_data *data = get_data();
    
    if (!data->initialized) {
        return 0;
    }
    
    return data->interrupt_count;
}

// 手动触发状态检查
void charging_monitor_force_check(void)
{
    struct charging_monitor_data *data = get_data();
    
    if (!data->initialized || !data->polling_active) {
        return;
    }
    
    LOG_DBG("Manual state check triggered");
    record_activity();
    
    k_work_cancel_delayable(&data->status_check_work);
    k_work_reschedule(&data->status_check_work, K_NO_WAIT);
}