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
    };
    return &data;
}

// 闪烁工作函数
static void blink_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct led_controller_data *data = CONTAINER_OF(dwork, struct led_controller_data, blink_work);
    
    if (!data->blinking_active) {
        return;
    }
    
    data->led_on = !data->led_on;
    
    if (data->led_on) {
        zmk_backlight_on();
    } else {
        zmk_backlight_off();
    }
    
    k_work_reschedule(dwork, K_MSEC(data->blink_interval_ms));
}

// 停止闪烁
static void stop_blinking(struct led_controller_data *data)
{
    if (data->blinking_active) {
        k_work_cancel_delayable(&data->blink_work);
        data->blinking_active = false;
        data->led_on = false;
        LOG_DBG("Blinking stopped");
    }
}

// 开始闪烁
static void start_blinking(struct led_controller_data *data, uint32_t interval_ms)
{
    if (interval_ms == 0) {
        LOG_ERR("Invalid blink interval: 0ms");
        return;
    }
    
    stop_blinking(data);
    
    data->blink_interval_ms = interval_ms;
    data->blinking_active = true;
    
    data->led_on = true;
    zmk_backlight_on();
    
    k_work_reschedule(&data->blink_work, K_MSEC(interval_ms / 2));
    
    LOG_DBG("Blinking started with interval: %dms", interval_ms);
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
    
    k_work_init_delayable(&data->blink_work, blink_work_handler);
    
    zmk_backlight_off();
    data->current_state = SYSTEM_LED_STATE_OFF;
    data->current_mode = LED_MODE_OFF;
    data->led_on = false;
    
    data->initialized = true;
    LOG_INF("LED controller initialized successfully");
    
    return 0;
}

// 设置LED状态
void led_controller_set_state(system_led_state_t state, led_mode_t mode, uint32_t interval_ms)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        LOG_ERR("LED controller not initialized");
        return;
    }
    
    if (state == data->current_state && mode == data->current_mode) {
        return;
    }
    
    data->current_state = state;
    data->current_mode = mode;
    
    switch (mode) {
    case LED_MODE_OFF:
        stop_blinking(data);
        zmk_backlight_off();
        data->led_on = false;
        LOG_DBG("LED set to OFF");
        break;
        
    case LED_MODE_ON:
        stop_blinking(data);
        zmk_backlight_on();
        data->led_on = true;
        LOG_DBG("LED set to ON");
        break;
        
    case LED_MODE_BLINK_SLOW:
    case LED_MODE_BLINK_FAST:
        start_blinking(data, interval_ms);
        LOG_DBG("LED set to BLINK (interval: %dms)", interval_ms);
        break;
        
    case LED_MODE_PULSE:
        LOG_WRN("PULSE mode not fully implemented, using blink instead");
        start_blinking(data, interval_ms * 2);
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