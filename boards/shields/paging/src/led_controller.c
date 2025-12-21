#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/backlight.h>

LOG_MODULE_REGISTER(led_controller, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"
#include "led_controller.h"

// LED控制器私有数据结构
struct led_controller_data {
    struct k_work_delayable blink_work;
    system_led_state_t current_state;
    led_mode_t current_mode;
    uint32_t blink_interval_ms;
    bool led_on;
    bool initialized;
    bool blinking_active;
    uint32_t blink_counter;  // 添加计数器用于调试
};

// 获取私有数据实例
static struct led_controller_data *get_data(void)
{
    static struct led_controller_data data = {
        .current_state = SYSTEM_LED_STATE_OFF,
        .current_mode = LED_MODE_OFF,
        .blink_interval_ms = CONFIG_BLUETOOTH_LED_BLINK_INTERVAL,
        .led_on = false,
        .initialized = false,
        .blinking_active = false,
        .blink_counter = 0,
    };
    return &data;
}

// 闪烁工作函数（修复版）
static void blink_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct led_controller_data *data = CONTAINER_OF(dwork, struct led_controller_data, blink_work);
    
    if (!data->blinking_active) {
        LOG_WRN("Blink work called but blinking not active");
        return;
    }
    
    // 切换LED状态
    data->led_on = !data->led_on;
    data->blink_counter++;
    
    if (data->led_on) {
        zmk_backlight_on();
        LOG_DBG("LED ON (blink count: %u)", data->blink_counter);
    } else {
        zmk_backlight_off();
        LOG_DBG("LED OFF (blink count: %u)", data->blink_counter);
    }
    
    // 关键修复：重新调度下一次闪烁
    // 使用相同的间隔，而不是一半间隔
    if (data->blinking_active) {
        k_work_reschedule(dwork, K_MSEC(data->blink_interval_ms));
        LOG_DBG("Next blink scheduled in %u ms", data->blink_interval_ms);
    }
}

// 停止闪烁
static void stop_blinking(struct led_controller_data *data)
{
    if (data->blinking_active) {
        LOG_DBG("Stopping blinking, counter was: %u", data->blink_counter);
        k_work_cancel_delayable(&data->blink_work);
        data->blinking_active = false;
        data->blink_counter = 0;
        data->led_on = false;
    }
}

// 开始闪烁（修复版）
static void start_blinking(struct led_controller_data *data, uint32_t interval_ms)
{
    if (interval_ms == 0) {
        LOG_ERR("Invalid blink interval: 0ms");
        return;
    }
    
    // 停止现有的闪烁
    stop_blinking(data);
    
    data->blink_interval_ms = interval_ms;
    data->blinking_active = true;
    data->blink_counter = 0;
    
    // 关键修复：立即开始第一次亮起
    data->led_on = true;
    zmk_backlight_on();
    LOG_DBG("Blinking started with interval: %dms, LED ON", interval_ms);
    
    // 关键修复：调度第一次熄灭
    // 这里应该使用 interval_ms/2 作为第一次切换的时间，实现亮-灭-亮-灭的循环
    k_work_reschedule(&data->blink_work, K_MSEC(interval_ms / 2));
    LOG_DBG("First toggle scheduled in %u ms", interval_ms / 2);
}

// 初始化LED控制器
int led_controller_init(void)
{
    struct led_controller_data *data = get_data();
    
    if (data->initialized) {
        LOG_WRN("LED controller already initialized");
        return 0;
    }
    
    LOG_INF("Initializing LED controller");
    
    // 初始化工作队列
    k_work_init_delayable(&data->blink_work, blink_work_handler);
    
    // 初始状态：熄灭
    zmk_backlight_off();
    data->current_state = SYSTEM_LED_STATE_OFF;
    data->current_mode = LED_MODE_OFF;
    data->led_on = false;
    data->blink_counter = 0;
    
    data->initialized = true;
    LOG_INF("LED controller initialized successfully");
    
    return 0;
}

// 设置LED状态（修复版）
void led_controller_set_state(system_led_state_t state, led_mode_t mode, uint32_t interval_ms)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        LOG_ERR("LED controller not initialized");
        return;
    }
    
    // 记录旧状态用于日志
    const char *old_state_str = 
        (data->current_state == SYSTEM_LED_STATE_CHARGING) ? "CHARGING" :
        (data->current_state == SYSTEM_LED_STATE_FULL_CHARGE) ? "FULL_CHARGE" :
        (data->current_state == SYSTEM_LED_STATE_BT_CONNECTED) ? "BT_CONNECTED" :
        (data->current_state == SYSTEM_LED_STATE_BT_DISCONNECTED) ? "BT_DISCONNECTED" :
        (data->current_state == SYSTEM_LED_STATE_ERROR) ? "ERROR" : "OFF";
    
    const char *new_state_str = 
        (state == SYSTEM_LED_STATE_CHARGING) ? "CHARGING" :
        (state == SYSTEM_LED_STATE_FULL_CHARGE) ? "FULL_CHARGE" :
        (state == SYSTEM_LED_STATE_BT_CONNECTED) ? "BT_CONNECTED" :
        (state == SYSTEM_LED_STATE_BT_DISCONNECTED) ? "BT_DISCONNECTED" :
        (state == SYSTEM_LED_STATE_ERROR) ? "ERROR" : "OFF";
    
    // 状态没有变化，直接返回
    if (state == data->current_state && mode == data->current_mode) {
        LOG_DBG("LED state unchanged: %s (mode: %d)", new_state_str, mode);
        return;
    }
    
    LOG_INF("LED state changing: %s -> %s (mode: %d, interval: %dms)",
           old_state_str, new_state_str, mode, interval_ms);
    
    // 更新状态
    data->current_state = state;
    data->current_mode = mode;
    
    // 根据模式控制LED
    switch (mode) {
    case LED_MODE_OFF:
        stop_blinking(data);
        zmk_backlight_off();
        data->led_on = false;
        LOG_INF("LED set to OFF");
        break;
        
    case LED_MODE_ON:
        stop_blinking(data);
        zmk_backlight_on();
        data->led_on = true;
        LOG_INF("LED set to ON");
        break;
        
    case LED_MODE_BLINK_SLOW:
    case LED_MODE_BLINK_FAST:
        start_blinking(data, interval_ms);
        LOG_INF("LED set to BLINK mode (interval: %dms)", interval_ms);
        break;
        
    case LED_MODE_PULSE:
        // 呼吸效果暂时实现为快速闪烁
        LOG_WRN("PULSE mode not fully implemented, using fast blink instead");
        start_blinking(data, interval_ms / 2);
        LOG_INF("LED set to PULSE (fast blink, interval: %dms)", interval_ms / 2);
        break;
        
    default:
        LOG_ERR("Unknown LED mode: %d", mode);
        stop_blinking(data);
        zmk_backlight_off();
        break;
    }
}

// 停止所有LED活动
void led_controller_stop_all(void)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    stop_blinking(data);
    zmk_backlight_off();
    data->current_state = SYSTEM_LED_STATE_OFF;
    data->current_mode = LED_MODE_OFF;
    data->led_on = false;
    
    LOG_INF("All LED activities stopped");
}