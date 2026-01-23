/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the current number of active layers
 * 
 * @param dev Pointer to the layer indicator device
 * @param count Pointer to store the active layer count
 * @return 0 on success, negative error code on failure
 */
int layer_indicator_get_active_count(const struct device *dev, uint8_t *count);

/**
 * @brief Get the highest active layer number
 * 
 * @param dev Pointer to the layer indicator device
 * @param layer Pointer to store the highest active layer number
 * @return 0 on success, negative error code on failure
 */
int layer_indicator_get_highest_layer(const struct device *dev, uint8_t *layer);

/**
 * @brief Check if a specific layer is active
 * 
 * @param dev Pointer to the layer indicator device
 * @param layer Layer number to check
 * @param active Pointer to store the result (true if active)
 * @return 0 on success, negative error code on failure
 */
int layer_indicator_is_layer_active(const struct device *dev, uint8_t layer, bool *active);

/**
 * @brief Get all active layers
 * 
 * @param dev Pointer to the layer indicator device
 * @param layers Array to store active layer numbers
 * @param max_layers Maximum number of layers that can be stored
 * @param count Pointer to store the actual number of active layers found
 * @return 0 on success, negative error code on failure
 */
int layer_indicator_get_all_active(const struct device *dev, 
                                  uint8_t *layers, 
                                  uint8_t max_layers, 
                                  uint8_t *count);

/**
 * @brief Get the timestamp of the last layer state change
 * 
 * @param dev Pointer to the layer indicator device
 * @param timestamp Pointer to store the timestamp
 * @return 0 on success, negative error code on failure
 */
int layer_indicator_get_last_change_time(const struct device *dev, int64_t *timestamp);

#ifdef __cplusplus
}
#endif  