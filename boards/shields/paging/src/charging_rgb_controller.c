#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(charging_rgb, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/rgb_underglow.h>
#include "charging_monitor.h"

// 充电状态变化回调函数
static void on_charging_state_changed(enum charging_state new_state)
{
    switch (new_state) {
    case CHARGING_STATE_CHARGING:
        LOG_INF("Charging detected - Turning RGB underglow ON");
        zmk_rgb_underglow_on();
        break;
        
    case CHARGING_STATE_DISCHARGING:
    case CHARGING_STATE_FULL:
        LOG_INF("Not charging - Turning RGB underglow OFF");
        zmk_rgb_underglow_off();
        break;
        
    case CHARGING_STATE_ERROR:
        LOG_WRN("Charging state error - Leaving RGB underglow as is");
        break;
    }
}

// 初始化RGB控制器
static int charging_rgb_controller_init(void)
{
    int ret;
    
    // 初始化充电监控器
    ret = charging_monitor_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize charging monitor: %d", ret);
        return ret;
    }
    
    // 注册回调到充电监控器
    ret = charging_monitor_register_callback(on_charging_state_changed);
    if (ret != 0) {
        LOG_ERR("Failed to register RGB underglow callback: %d", ret);
        return ret;
    }
    
    LOG_INF("Charging RGB underglow controller initialized");
    return 0;
}

// 只有在启用RGB充电控制时才编译此初始化
#ifdef CONFIG_ZMK_CHARGING_RGB_CONTROL
SYS_INIT(charging_rgb_controller_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif