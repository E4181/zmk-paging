#ifndef ZMK_CHARGING_STATUS_H
#define ZMK_CHARGING_STATUS_H

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 充电状态枚举 */
enum charging_status_state {
    CHARGING_STATUS_NOT_CHARGING = 0,
    CHARGING_STATUS_CHARGING,
    CHARGING_STATUS_UNKNOWN
};

/* 设备API函数指针类型 */
typedef enum charging_status_state (*charging_status_get_state_t)(const struct device *dev);
typedef int (*charging_status_set_callback_t)(const struct device *dev, 
                                             void (*callback)(const struct device *dev, 
                                                             enum charging_status_state state));
typedef int (*charging_status_set_interrupt_t)(const struct device *dev, bool enable);
typedef int (*charging_status_update_t)(const struct device *dev);

/* 设备API结构体 */
struct charging_status_driver_api {
    charging_status_get_state_t get_state;
    charging_status_set_callback_t set_callback;
    charging_status_set_interrupt_t set_interrupt;
    charging_status_update_t update;
};

/* API函数 */
static inline enum charging_status_state charging_status_get_state(const struct device *dev)
{
    const struct charging_status_driver_api *api = dev->api;
    return api->get_state(dev);
}

static inline int charging_status_set_callback(const struct device *dev, 
                                              void (*callback)(const struct device *dev, 
                                                              enum charging_status_state state))
{
    const struct charging_status_driver_api *api = dev->api;
    return api->set_callback(dev, callback);
}

static inline int charging_status_set_interrupt(const struct device *dev, bool enable)
{
    const struct charging_status_driver_api *api = dev->api;
    return api->set_interrupt(dev, enable);
}

static inline int charging_status_update(const struct device *dev)
{
    const struct charging_status_driver_api *api = dev->api;
    return api->update(dev);
}

#ifdef __cplusplus
}
#endif

#endif /* ZMK_CHARGING_STATUS_H */