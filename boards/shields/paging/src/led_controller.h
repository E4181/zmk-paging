#pragma once

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "state_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

// LED控制器API
int led_controller_init(void);
void led_controller_set_state(system_led_state_t state, led_mode_t mode, uint32_t interval_ms);
void led_controller_stop_all(void);
void led_controller_on(void);
void led_controller_off(void);
void led_controller_toggle(void);
bool led_controller_is_on(void);

#ifdef __cplusplus
}
#endif