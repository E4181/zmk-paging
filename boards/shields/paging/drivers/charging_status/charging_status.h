#pragma once

#include <stdbool.h>

/**
 * @brief Check whether device is currently charging
 *
 * @retval true   Charging (TP4056 CHRG = low)
 * @retval false  Not charging
 */
bool charging_status_is_charging(void);
