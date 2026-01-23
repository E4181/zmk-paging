#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zmk/event_manager.h>
#include <zmk/events/ble_connection_state_changed.h>

#include "bluetooth_led.h"

LOG_MODULE_REGISTER(bluetooth_led, CONFIG_ZMK_LOG_LEVEL);

/* ===== LED 定义 ===== */
#define LED_NODE DT_ALIAS(led0)

#if !DT_NODE_HAS_STATUS(LED_NODE, okay)
#error "LED alias 'led0' not defined in device tree"
#endif

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* ===== 状态 ===== */
static atomic_t ble_connected = ATOMIC_INIT(0);
static atomic_t led_on = ATOMIC_INIT(0);

/* ===== 定时器 ===== */
#define BLINK_INTERVAL_MS 500

static struct k_timer blink_timer;

/* ===== LED 操作 ===== */
static inline void led_set(bool on) {
    gpio_pin_set_dt(&led, on);
    atomic_set(&led_on, on);
}

static void blink_timer_handler(struct k_timer *timer) {
    if (atomic_get(&ble_connected)) {
        /* 已连接：确保 LED 关闭 */
        led_set(false);
        return;
    }

    /* 未连接：翻转 LED */
    led_set(!atomic_get(&led_on));
}

/* ===== 蓝牙事件监听 ===== */
static int bluetooth_led_event_listener(const zmk_event_t *eh) {

    if (as_zmk_ble_connection_state_changed(eh)) {
        const struct zmk_ble_connection_state_changed *ev =
            cast_zmk_ble_connection_state_changed(eh);

        if (ev->connected) {
            atomic_set(&ble_connected, 1);
            led_set(false);
            LOG_INF("BLE connected, LED off");
        } else {
            atomic_set(&ble_connected, 0);
            LOG_INF("BLE disconnected, LED blinking");
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bluetooth_led, bluetooth_led_event_listener);
ZMK_SUBSCRIPTION(bluetooth_led, zmk_ble_connection_state_changed);

/* ===== 初始化 ===== */
static int bluetooth_led_init(void) {

    if (!device_is_ready(led.port)) {
        LOG_ERR("LED GPIO device not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    k_timer_init(&blink_timer, blink_timer_handler, NULL);
    k_timer_start(&blink_timer,
                  K_MSEC(BLINK_INTERVAL_MS),
                  K_MSEC(BLINK_INTERVAL_MS));

    LOG_INF("Bluetooth LED driver initialized");

    return 0;
}

SYS_INIT(bluetooth_led_init, APPLICATION, 90);

/* ===== 外部接口 ===== */
bool bluetooth_led_is_connected(void) {
    return atomic_get(&ble_connected);
}
