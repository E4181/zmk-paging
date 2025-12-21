#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

LOG_MODULE_REGISTER(charging_backlight, CONFIG_ZMK_LOG_LEVEL);

#include "config.h"

// 条件编译包含
#if ENABLE_CHARGING_MONITOR
#include "charging_monitor.h"
#endif

#if ENABLE_BLUETOOTH_LED
#include "bluetooth_monitor.h"
#endif

#if ENABLE_STATE_COORDINATOR
#include "state_coordinator.h"
#endif

#if ENABLE_LED_CONTROLLER
#include "led_controller.h"
#endif

static struct k_work_delayable init_work;

// 条件编译的回调函数
#if ENABLE_CHARGING_MONITOR
static void on_charging_state_changed(charging_state_t new_state)
{
    LOG_INF("Charging state changed: %d", new_state);
    #if ENABLE_STATE_COORDINATOR
    state_coordinator_update_charging(new_state);
    #else
    // 如果没有状态协调器，直接处理
    if (new_state == CHARGING_STATE_CHARGING) {
        #if ENABLE_LED_CONTROLLER
        led_controller_set_state(SYSTEM_LED_STATE_CHARGING, LED_MODE_ON, 0);
        #endif
    } else if (new_state == CHARGING_STATE_FULL) {
        #if ENABLE_LED_CONTROLLER
        led_controller_set_state(SYSTEM_LED_STATE_FULL_CHARGE, LED_MODE_OFF, 0);
        #endif
    }
    #endif
}
#endif

#if ENABLE_BLUETOOTH_LED
static void on_bluetooth_state_changed(bluetooth_state_t new_state)
{
    LOG_INF("Bluetooth state changed: %d", new_state);
    #if ENABLE_STATE_COORDINATOR
    state_coordinator_update_bluetooth(new_state);
    #endif
}
#endif

#if ENABLE_STATE_COORDINATOR
static void on_system_state_changed(system_led_state_t new_state, led_mode_t mode, uint32_t interval_ms)
{
    LOG_INF("System state changed: %d, mode: %d, interval: %d", new_state, mode, interval_ms);
    #if ENABLE_LED_CONTROLLER
    led_controller_set_state(new_state, mode, interval_ms);
    #endif
}
#endif

// 测试函数：手动测试LED闪烁
static void test_led_blink(struct k_work *work)
{
    ARG_UNUSED(work);
    
    #if ENABLE_LED_CONTROLLER
    static bool test_on = false;
    
    if (!test_on) {
        LOG_INF("=== TEST: Starting LED blink ===");
        led_controller_set_state(SYSTEM_LED_STATE_BT_DISCONNECTED, 
                                LED_MODE_BLINK_SLOW, 
                                CONFIG_BLUETOOTH_LED_BLINK_INTERVAL);
        test_on = true;
    } else {
        LOG_INF("=== TEST: Stopping LED blink ===");
        led_controller_set_state(SYSTEM_LED_STATE_OFF, LED_MODE_OFF, 0);
        test_on = false;
    }
    
    // 10秒后再次测试
    k_work_reschedule(&init_work, K_SECONDS(10));
    #endif
}

// 延迟初始化工作函数
static void delayed_init_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    
    int ret;
    
    LOG_INF("Initializing custom LED controller system");
    
    // 初始化LED控制器
    #if ENABLE_LED_CONTROLLER
    ret = led_controller_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize LED controller: %d", ret);
        return;
    }
    #endif
    
    // 初始化状态协调器
    #if ENABLE_STATE_COORDINATOR
    ret = state_coordinator_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize state coordinator: %d", ret);
        return;
    }
    
    #if ENABLE_LED_CONTROLLER
    ret = state_coordinator_register_callback(on_system_state_changed);
    if (ret != 0) {
        LOG_ERR("Failed to register system state callback: %d", ret);
    }
    #endif
    #endif
    
    // 初始化充电监控器
    #if ENABLE_CHARGING_MONITOR
    ret = charging_monitor_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize charging monitor: %d", ret);
    } else {
        ret = charging_monitor_register_callback(on_charging_state_changed);
        if (ret != 0) {
            LOG_ERR("Failed to register charging callback: %d", ret);
        }
    }
    #endif
    
    // 初始化蓝牙监控器
    #if ENABLE_BLUETOOTH_LED
    ret = bluetooth_monitor_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize bluetooth monitor: %d", ret);
        LOG_WRN("Continuing without bluetooth monitoring");
    } else {
        ret = bluetooth_monitor_register_callback(on_bluetooth_state_changed);
        if (ret != 0) {
            LOG_ERR("Failed to register bluetooth callback: %d", ret);
        }
    }
    #endif
    
    LOG_INF("Custom LED controller system initialization completed");
    
    // 启动LED闪烁测试
    #if ENABLE_LED_CONTROLLER
    LOG_INF("Starting LED test in 2 seconds...");
    k_work_reschedule(&init_work, K_SECONDS(2));
    #endif
}

// 初始化背光控制器
static int charging_backlight_controller_init(void)
{
    // 延迟3秒初始化，确保键盘功能先启动
    k_work_init_delayable(&init_work, delayed_init_work_handler);
    k_work_reschedule(&init_work, K_SECONDS(3));
    
    LOG_INF("Custom LED controller system scheduled for initialization");
    return 0;
}

// Zephyr系统初始化
SYS_INIT(charging_backlight_controller_init, APPLICATION, 99);