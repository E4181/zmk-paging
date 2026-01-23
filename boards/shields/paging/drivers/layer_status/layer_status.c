#include <device.h>
#include <drivers/led.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>
#include <devicetree.h>

// 获取 DTS 中配置的层号
#define BLUE_LAYER DT_PROP(DT_PATH(layer_colors), blue_layer)
#define YELLOW_LAYER DT_PROP(DT_PATH(layer_colors), yellow_layer)

static const struct device *led_dev;

// 设置指定颜色
static void set_led_color(uint8_t red, uint8_t green, uint8_t blue) {
    if (!led_dev) {
        led_dev = device_get_binding("layer_status_led");
        if (!led_dev) {
            return;
        }
    }

    struct led_rgb color = { .red = red, .green = green, .blue = blue };
    led_rgb_set(led_dev, 0, &color); // 0 表示第 1 个 LED
}

// 根据层号切换颜色
static void update_layer_color(uint8_t layer) {
    switch (layer) {
        case BLUE_LAYER:
            set_led_color(0, 0, 255);      // 蓝光
            break;
        case YELLOW_LAYER:
            set_led_color(255, 255, 0);    // 黄光
            break;
        default:
            set_led_color(0, 0, 0);        // 关闭
            break;
    }
}

// 事件回调
static void layer_state_changed_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *event = as_zmk_layer_state_changed(eh);
    if (event->state) {  // 层被激活
        update_layer_color(event->layer);
    }
}

ZMK_LISTENER(layer_status_listener, layer_state_changed_listener);
ZMK_SUBSCRIPTION(layer_status_listener, zmk_layer_state_changed);
