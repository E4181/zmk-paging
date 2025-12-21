#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

LOG_MODULE_REGISTER(led_controller, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"
#include "led_controller.h"

// 硬编码的引脚配置（使用P0.26）
#define STATUS_LED_PORT DT_NODELABEL(gpio0)  // GPIO0
#define STATUS_LED_PIN 26                     // P0.26
#define STATUS_LED_FLAGS (GPIO_OUTPUT | (CONFIG_STATUS_LED_ACTIVE_HIGH ? GPIO_ACTIVE_HIGH : GPIO_ACTIVE_LOW))

// LED控制器私有数据结构
struct led_controller_data {
    struct k_work_delayable blink_work;
    system_led_state_t current_state;
    led_mode_t current_mode;
    uint32_t blink_interval_ms;
    bool led_on;
    bool initialized;
    bool blinking_active;
    uint32_t blink_counter;
    
    // GPIO相关
    const struct device *gpio_dev;
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
        .gpio_dev = NULL,
    };
    return &data;
}

// 设置GPIO电平
static void set_gpio_level(bool level)
{
    struct led_controller_data *data = get_data();
    
    if (!data->gpio_dev || !device_is_ready(data->gpio_dev)) {
        LOG_ERR("GPIO device not ready");
        return;
    }
    
    int ret = gpio_pin_set(data->gpio_dev, STATUS_LED_PIN, level ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to set GPIO pin %d: %d", STATUS_LED_PIN, ret);
    }
}

// 闪烁工作函数
static void blink_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct led_controller_data *data = CONTAINER_OF(dwork, struct led_controller_data, blink_work);
    
    if (!data->blinking_active) {
        return;
    }
    
    // 切换LED状态
    data->led_on = !data->led_on;
    data->blink_counter++;
    
    if (data->led_on) {
        set_gpio_level(true);
    } else {
        set_gpio_level(false);
    }
    
    // 重新调度下一次闪烁
    if (data->blinking_active) {
        k_work_reschedule(dwork, K_MSEC(data->blink_interval_ms));
    }
}

// 停止闪烁
static void stop_blinking(struct led_controller_data *data)
{
    if (data->blinking_active) {
        k_work_cancel_delayable(&data->blink_work);
        data->blinking_active = false;
        data->blink_counter = 0;
        data->led_on = false;
    }
}

// 开始闪烁
static void start_blinking(struct led_controller_data *data, uint32_t interval_ms)
{
    if (interval_ms == 0) {
        return;
    }
    
    // 停止现有的闪烁
    stop_blinking(data);
    
    data->blink_interval_ms = interval_ms;
    data->blinking_active = true;
    data->blink_counter = 0;
    
    // 立即开始第一次亮起
    data->led_on = true;
    set_gpio_level(true);
    
    // 调度第一次熄灭（间隔为半周期）
    k_work_reschedule(&data->blink_work, K_MSEC(interval_ms / 2));
}

// 初始化LED控制器
int led_controller_init(void)
{
    struct led_controller_data *data = get_data();
    int ret;
    
    if (data->initialized) {
        return 0;
    }
    
    LOG_INF("Initializing custom LED controller on GPIO0 pin %d (P0.26)", STATUS_LED_PIN);
    
    // 获取GPIO设备
    data->gpio_dev = DEVICE_DT_GET(STATUS_LED_PORT);
    if (!data->gpio_dev) {
        LOG_ERR("Failed to get GPIO0 device");
        return -ENODEV;
    }
    
    if (!device_is_ready(data->gpio_dev)) {
        LOG_ERR("GPIO0 device not ready");
        return -ENODEV;
    }
    
    // 配置GPIO引脚
    ret = gpio_pin_configure(data->gpio_dev, STATUS_LED_PIN, STATUS_LED_FLAGS);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO pin %d: %d", STATUS_LED_PIN, ret);
        return ret;
    }
    
    LOG_INF("Custom LED controller configured: GPIO0 pin %d, active %s",
           STATUS_LED_PIN, CONFIG_STATUS_LED_ACTIVE_HIGH ? "HIGH" : "LOW");
    
    // 初始化工作队列
    k_work_init_delayable(&data->blink_work, blink_work_handler);
    
    // 初始状态：熄灭
    set_gpio_level(false);
    data->current_state = SYSTEM_LED_STATE_OFF;
    data->current_mode = LED_MODE_OFF;
    data->led_on = false;
    data->blink_counter = 0;
    
    data->initialized = true;
    LOG_INF("Custom LED controller initialized successfully");
    
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
    
    // 状态没有变化，直接返回
    if (state == data->current_state && mode == data->current_mode) {
        return;
    }
    
    // 更新状态
    data->current_state = state;
    data->current_mode = mode;
    
    // 根据模式控制LED
    switch (mode) {
    case LED_MODE_OFF:
        stop_blinking(data);
        set_gpio_level(false);
        data->led_on = false;
        LOG_INF("LED set to OFF");
        break;
        
    case LED_MODE_ON:
        stop_blinking(data);
        set_gpio_level(true);
        data->led_on = true;
        LOG_INF("LED set to ON");
        break;
        
    case LED_MODE_BLINK_SLOW:
    case LED_MODE_BLINK_FAST:
        start_blinking(data, interval_ms);
        LOG_INF("LED set to BLINK mode (interval: %dms)", interval_ms);
        break;
        
    case LED_MODE_PULSE:
        LOG_WRN("PULSE mode not implemented, using blink instead");
        start_blinking(data, interval_ms / 2);
        break;
        
    default:
        stop_blinking(data);
        set_gpio_level(false);
        break;
    }
}

// 手动打开LED
void led_controller_on(void)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    stop_blinking(data);
    set_gpio_level(true);
    data->led_on = true;
    data->current_state = SYSTEM_LED_STATE_CHARGING;
    data->current_mode = LED_MODE_ON;
}

// 手动关闭LED
void led_controller_off(void)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    stop_blinking(data);
    set_gpio_level(false);
    data->led_on = false;
    data->current_state = SYSTEM_LED_STATE_OFF;
    data->current_mode = LED_MODE_OFF;
}

// 手动切换LED
void led_controller_toggle(void)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    stop_blinking(data);
    data->led_on = !data->led_on;
    set_gpio_level(data->led_on);
    data->current_state = data->led_on ? SYSTEM_LED_STATE_CHARGING : SYSTEM_LED_STATE_OFF;
    data->current_mode = data->led_on ? LED_MODE_ON : LED_MODE_OFF;
}

// 检查LED是否亮起
bool led_controller_is_on(void)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        return false;
    }
    
    return data->led_on;
}

// 停止所有LED活动
void led_controller_stop_all(void)
{
    struct led_controller_data *data = get_data();
    
    if (!data->initialized) {
        return;
    }
    
    stop_blinking(data);
    set_gpio_level(false);
    data->current_state = SYSTEM_LED_STATE_OFF;
    data->current_mode = LED_MODE_OFF;
    data->led_on = false;
    
    LOG_INF("All LED activities stopped");
}