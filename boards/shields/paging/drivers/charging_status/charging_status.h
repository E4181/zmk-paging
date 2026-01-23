/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * Charging Status Driver API
 */

#ifndef ZEPHYR_DRIVERS_CHARGING_STATUS_H_
#define ZEPHYR_DRIVERS_CHARGING_STATUS_H_

#include <zephyr/device.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback function type for charging status changes
 * @param is_charging True if currently charging, false otherwise
 * @param user_data User provided context
 */
typedef void (*charging_status_callback_t)(bool is_charging, void *user_data);

/**
 * @brief Charging status driver API structure
 */
__subsystem struct charging_status_driver_api {
    /**
     * @brief Get current charging status
     * @param dev Charging status device
     * @param is_charging Pointer to store charging status
     * @return 0 on success, negative error code on failure
     */
    int (*get_status)(const struct device *dev, bool *is_charging);
    
    /**
     * @brief Register callback for charging status changes
     * @param dev Charging status device
     * @param callback Callback function
     * @param user_data User data passed to callback
     * @return 0 on success, negative error code on failure
     */
    int (*register_callback)(const struct device *dev,
                           charging_status_callback_t callback,
                           void *user_data);
    
    /**
     * @brief Get timestamp of last status change
     * @param dev Charging status device
     * @param timestamp Pointer to store timestamp (ms since boot)
     * @return 0 on success, negative error code on failure
     */
    int (*get_last_change)(const struct device *dev, int64_t *timestamp);
};

/**
 * @brief Get current charging status
 * @param dev Charging status device
 * @param is_charging Pointer to store charging status
 * @return 0 on success, negative error code on failure
 */
__syscall int charging_status_get(const struct device *dev, bool *is_charging);

static inline int z_impl_charging_status_get(const struct device *dev, bool *is_charging)
{
    const struct charging_status_driver_api *api =
        (const struct charging_status_driver_api *)dev->api;
    
    return api->get_status(dev, is_charging);
}

/**
 * @brief Register callback for charging status changes
 * @param dev Charging status device
 * @param callback Callback function
 * @param user_data User data passed to callback
 * @return 0 on success, negative error code on failure
 */
__syscall int charging_status_register_callback(const struct device *dev,
                                              charging_status_callback_t callback,
                                              void *user_data);

static inline int z_impl_charging_status_register_callback(const struct device *dev,
                                                         charging_status_callback_t callback,
                                                         void *user_data)
{
    const struct charging_status_driver_api *api =
        (const struct charging_status_driver_api *)dev->api;
    
    return api->register_callback(dev, callback, user_data);
}

/**
 * @brief Get timestamp of last status change
 * @param dev Charging status device
 * @param timestamp Pointer to store timestamp (ms since boot)
 * @return 0 on success, negative error code on failure
 */
__syscall int charging_status_get_last_change(const struct device *dev,
                                            int64_t *timestamp);

static inline int z_impl_charging_status_get_last_change(const struct device *dev,
                                                       int64_t *timestamp)
{
    const struct charging_status_driver_api *api =
        (const struct charging_status_driver_api *)dev->api;
    
    return api->get_last_change(dev, timestamp);
}

#ifdef __cplusplus
}
#endif

#include <syscalls/charging_status.h>

#endif /* ZEPHYR_DRIVERS_CHARGING_STATUS_H_ */