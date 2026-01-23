/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * Charging Status Driver for TP4056
 * Uses interrupt mode for real-time charging status detection
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(charging_status, CONFIG_CHARGING_STATUS_LOG_LEVEL);

/* Driver data structure */
struct charging_status_data {
    /* GPIO callback structure */
    struct gpio_callback chrg_cb;
    
    /* Current charging status */
    atomic_t charging;
    
    /* Last status change timestamp */
    int64_t last_change_time;
    
    /* Debounce tracking */
    struct k_work_delayable debounce_work;
    
    /* Thread for monitoring */
    struct k_thread monitor_thread;
    
    /* Thread stack */
    k_thread_stack_t *thread_stack;
    
    /* Status update callback */
    charging_status_callback_t user_callback;
    void *user_callback_data;
};

/* Driver configuration structure */
struct charging_status_config {
    /* CHRG GPIO specification */
    struct gpio_dt_spec chrg_gpio;
    
    /* Polling interval */
    uint32_t status_interval_ms;
    
    /* Thread configuration */
    size_t thread_stack_size;
    int thread_priority;
};

/* Forward declarations */
static void charging_status_debounce_work(struct k_work *work);
static void charging_status_monitor_thread(void *p1, void *p2, void *p3);

/* GPIO interrupt callback */
static void charging_status_gpio_callback(const struct device *dev,
                                         struct gpio_callback *cb,
                                         uint32_t pins)
{
    struct charging_status_data *data = 
        CONTAINER_OF(cb, struct charging_status_data, chrg_cb);
    
    /* Schedule debounce work to handle potential bouncing */
    k_work_schedule(&data->debounce_work, K_MSEC(10));
}

/* Debounce work handler */
static void charging_status_debounce_work(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct charging_status_data *data = 
        CONTAINER_OF(dwork, struct charging_status_data, debounce_work);
    const struct device *dev = data->chrg_cb.dev;
    const struct charging_status_config *config = dev->config;
    
    int current_state = gpio_pin_get_dt(&config->chrg_gpio);
    atomic_t old_status;
    bool status_changed = false;
    
    /* TP4056 CHRG pin is active LOW when charging */
    int new_charging_status = (current_state == 0);
    int old_charging_status = atomic_get(&data->charging);
    
    if (new_charging_status != old_charging_status) {
        atomic_set(&data->charging, new_charging_status);
        data->last_change_time = k_uptime_get();
        status_changed = true;
        
        LOG_DBG("Charging status changed: %s",
                new_charging_status ? "CHARGING" : "NOT_CHARGING");
        
        /* Notify user callback if registered */
        if (data->user_callback) {
            data->user_callback(new_charging_status, data->user_callback_data);
        }
    }
}

/* Monitoring thread function */
static void charging_status_monitor_thread(void *p1, void *p2, void *p3)
{
    const struct device *dev = (const struct device *)p1;
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    
    LOG_DBG("Charging status monitor thread started");
    
    while (1) {
        /* Force a status check periodically */
        charging_status_debounce_work(&data->debounce_work.work);
        
        /* Sleep for the configured interval */
        k_msleep(config->status_interval_ms);
    }
}

/* Driver API: Get current charging status */
static int charging_status_get(const struct device *dev, bool *is_charging)
{
    struct charging_status_data *data = dev->data;
    
    if (is_charging == NULL) {
        return -EINVAL;
    }
    
    *is_charging = (atomic_get(&data->charging) != 0);
    return 0;
}

/* Driver API: Register callback for status changes */
static int charging_status_register_callback(const struct device *dev,
                                           charging_status_callback_t callback,
                                           void *user_data)
{
    struct charging_status_data *data = dev->data;
    
    if (callback == NULL) {
        return -EINVAL;
    }
    
    data->user_callback = callback;
    data->user_callback_data = user_data;
    
    return 0;
}

/* Driver API: Get last status change time */
static int charging_status_get_last_change(const struct device *dev,
                                          int64_t *timestamp)
{
    struct charging_status_data *data = dev->data;
    
    if (timestamp == NULL) {
        return -EINVAL;
    }
    
    *timestamp = data->last_change_time;
    return 0;
}

/* Driver API structure */
static const struct charging_status_driver_api charging_status_api = {
    .get_status = charging_status_get,
    .register_callback = charging_status_register_callback,
    .get_last_change = charging_status_get_last_change,
};

/* Driver initialization */
static int charging_status_init(const struct device *dev)
{
    struct charging_status_data *data = dev->data;
    const struct charging_status_config *config = dev->config;
    int ret;
    
    /* Validate GPIO device */
    if (!device_is_ready(config->chrg_gpio.port)) {
        LOG_ERR("CHRG GPIO device not ready");
        return -ENODEV;
    }
    
    /* Configure GPIO as input with interrupt */
    ret = gpio_pin_configure_dt(&config->chrg_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure CHRG GPIO: %d", ret);
        return ret;
    }
    
    /* Initialize GPIO callback */
    gpio_init_callback(&data->chrg_cb, charging_status_gpio_callback,
                      BIT(config->chrg_gpio.pin));
    
    /* Add callback for both edges (charging starts and stops) */
    ret = gpio_add_callback_dt(&config->chrg_gpio, &data->chrg_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback: %d", ret);
        return ret;
    }
    
    /* Configure interrupt for both rising and falling edges */
    ret = gpio_pin_interrupt_configure_dt(&config->chrg_gpio,
                                         GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO interrupt: %d", ret);
        return ret;
    }
    
    /* Initialize debounce work */
    k_work_init_delayable(&data->debounce_work, charging_status_debounce_work);
    
    /* Initial status read */
    int initial_state = gpio_pin_get_dt(&config->chrg_gpio);
    atomic_set(&data->charging, (initial_state == 0));
    data->last_change_time = k_uptime_get();
    
    LOG_INF("Charging status initialized: %s",
            (initial_state == 0) ? "CHARGING" : "NOT_CHARGING");
    
    /* Create monitoring thread */
    data->thread_stack = k_thread_stack_alloc(config->thread_stack_size,
                                             K_THREAD_STACK_SIZEOF(config->thread_stack_size));
    if (data->thread_stack == NULL) {
        LOG_ERR("Failed to allocate thread stack");
        return -ENOMEM;
    }
    
    k_thread_create(&data->monitor_thread,
                   data->thread_stack,
                   config->thread_stack_size,
                   charging_status_monitor_thread,
                   (void *)dev, NULL, NULL,
                   config->thread_priority,
                   0, K_NO_WAIT);
    
    return 0;
}

/* Helper macros for device instantiation */
#define CHARGING_STATUS_INIT(n)                                               \
    static struct charging_status_data charging_status_data_##n = {           \
        .charging = ATOMIC_INIT(0),                                           \
        .last_change_time = 0,                                                \
        .user_callback = NULL,                                                \
        .user_callback_data = NULL,                                           \
    };                                                                        \
                                                                              \
    static const struct charging_status_config charging_status_config_##n = { \
        .chrg_gpio = GPIO_DT_SPEC_INST_GET(n, chrg_gpios),                   \
        .status_interval_ms = DT_INST_PROP_OR(n, status_interval_ms, 1000),  \
        .thread_stack_size = CONFIG_CHARGING_STATUS_THREAD_STACK_SIZE,       \
        .thread_priority = CONFIG_CHARGING_STATUS_THREAD_PRIORITY,           \
    };                                                                        \
                                                                              \
    DEVICE_DT_INST_DEFINE(n,                                                  \
                         charging_status_init,                                \
                         NULL,                                                \
                         &charging_status_data_##n,                           \
                         &charging_status_config_##n,                         \
                         POST_KERNEL,                                         \
                         CONFIG_CHARGING_STATUS_INIT_PRIORITY,                \
                         &charging_status_api);

/* Instantiate all instances from device tree */
DT_INST_FOREACH_STATUS_OKAY(CHARGING_STATUS_INIT)