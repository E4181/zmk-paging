#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

/* 注册日志模块 */
LOG_MODULE_REGISTER(charging_status, LOG_LEVEL_INF);

/* ⚠ 必须定义 DT_DRV_COMPAT，对应 DTS compatible */
#define DT_DRV_COMPAT zmk_charging_status

/* 呼吸灯参数 */
#define BREATH_STEPS        64
#define BREATH_PERIOD_MS    20
#define PWM_PERIOD_USEC     1000

/* Gamma-like 查表，非线性亮度更自然 */
static const uint16_t breath_lut[BREATH_STEPS] = {
    0,2,5,9,15,22,31,42,
    55,70,88,109,133,160,190,223,
    259,298,340,385,433,484,538,595,
    655,717,781,846,911,976,1000,976,
    911,846,781,717,655,595,538,484,
    433,385,340,298,259,223,190,160,
    133,109,88,70,55,42,31,22,
    15,9,5,2
};

/* ===================== 数据结构 ===================== */
struct charging_status_config {
    struct gpio_dt_spec charge_gpio;
    struct pwm_dt_spec pwm;
};

struct charging_status_data {
    struct k_work_delayable breath_work;
    struct gpio_callback gpio_cb;
    uint8_t step;
    bool active;
};

/* ===================== 呼吸灯 Handler ===================== */
static void breath_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork =
        CONTAINER_OF(work, struct k_work_delayable, work);

    struct charging_status_data *data =
        CONTAINER_OF(dwork, struct charging_status_data, breath_work);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    const struct charging_status_config *cfg = dev->config;

    if (!data->active) {
        int ret = pwm_set_dt(&cfg->pwm, PWM_PERIOD_USEC, 0);
        if (ret < 0) {
            LOG_WRN("Failed to set PWM off: %d", ret);
        }
        LOG_INF("Breath LED off");
        return;
    }

    int ret = pwm_set_dt(&cfg->pwm, PWM_PERIOD_USEC, breath_lut[data->step]);
    if (ret < 0) {
        LOG_WRN("Failed to set PWM: %d", ret);
        // 发生错误时停止呼吸效果
        data->active = false;
        return;
    }

    data->step = (data->step + 1) % BREATH_STEPS;

    LOG_DBG("Breath LED step: %d", data->step);

    k_work_schedule(&data->breath_work, K_MSEC(BREATH_PERIOD_MS));
}

/* ===================== GPIO 中断 Handler ===================== */
static void charge_gpio_isr(const struct device *port,
                            struct gpio_callback *cb,
                            uint32_t pins)
{
    struct charging_status_data *data =
        CONTAINER_OF(cb, struct charging_status_data, gpio_cb);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    const struct charging_status_config *cfg = dev->config;

    // 获取逻辑电平
    int pin_state = gpio_pin_get_dt(&cfg->charge_gpio);
    
    // 逻辑电平为 1 表示充电（对于 GPIO_ACTIVE_LOW 就是物理低电平）
    bool is_charging = (pin_state > 0);

    // 不直接在中断中调度工作，只更新状态
    if (is_charging) {
        if (!data->active) {
            data->active = true;
            data->step = 0;
            LOG_INF("Charging detected, will start breath LED");
        }
    } else {
        if (data->active) {
            data->active = false;
            LOG_INF("Charging stopped, will stop breath LED");
        }
    }
}

/* ===================== 呼吸灯 Handler ===================== */
static void breath_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork =
        CONTAINER_OF(work, struct k_work_delayable, work);

    struct charging_status_data *data =
        CONTAINER_OF(dwork, struct charging_status_data, breath_work);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    const struct charging_status_config *cfg = dev->config;

    // 首先读取当前GPIO状态，确保状态同步
    int pin_state = gpio_pin_get_dt(&cfg->charge_gpio);
    bool is_charging = (pin_state > 0);
    
    // 如果状态不一致，更新状态
    if (is_charging != data->active) {
        LOG_DBG("State mismatch: GPIO=%d, active=%d, updating...", 
                pin_state, data->active);
        data->active = is_charging;
        if (is_charging) {
            data->step = 0;
        }
    }

    if (!data->active) {
        pwm_set_dt(&cfg->pwm, PWM_PERIOD_USEC, 0);
        LOG_DBG("Breath LED off");
    } else {
        pwm_set_dt(&cfg->pwm, PWM_PERIOD_USEC, breath_lut[data->step]);
        data->step = (data->step + 1) % BREATH_STEPS;
        LOG_DBG("Breath LED step: %d", data->step);
        
        // 只有充电时才继续调度
        k_work_schedule(&data->breath_work, K_MSEC(BREATH_PERIOD_MS));
    }
}

/* ===================== 初始化 ===================== */
static int charging_status_init(const struct device *dev)
{
    const struct charging_status_config *cfg = dev->config;
    struct charging_status_data *data = dev->data;

    if (!device_is_ready(cfg->pwm.dev)) {
        LOG_ERR("PWM device not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&cfg->charge_gpio)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }

    /* 配置 GPIO 输入 */
    int ret = gpio_pin_configure_dt(&cfg->charge_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO: %d", ret);
        return ret;
    }

    /* 恢复为边沿触发中断 - 避免中断风暴 */
    ret = gpio_pin_interrupt_configure_dt(&cfg->charge_gpio,
                                          GPIO_INT_EDGE_BOTH);
    if (ret < 0) {
        LOG_ERR("Failed to configure interrupt: %d", ret);
        return ret;
    }

    gpio_init_callback(&data->gpio_cb,
                       charge_gpio_isr,
                       BIT(cfg->charge_gpio.pin));
    
    ret = gpio_add_callback(cfg->charge_gpio.port, &data->gpio_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add callback: %d", ret);
        return ret;
    }

    /* 初始化 work */
    k_work_init_delayable(&data->breath_work, breath_work_handler);

    data->active = false;
    data->step = 0;

    /* 关闭PWM输出，确保初始状态为关闭 */
    pwm_set_dt(&cfg->pwm, PWM_PERIOD_USEC, 0);
    
    /* 延迟初始状态读取，避免在系统初始化关键期执行 */
    k_work_schedule(&data->breath_work, K_MSEC(100));

    LOG_INF("Charging status driver initialized");

    return 0;
}

/* ===================== 实例化 ===================== */
#define CHARGING_STATUS_DEFINE(inst)                          \
static struct charging_status_data data_##inst;              \
static const struct charging_status_config cfg_##inst = {    \
    .charge_gpio = GPIO_DT_SPEC_INST_GET(inst, charge_gpios),\
    .pwm = PWM_DT_SPEC_INST_GET(inst),                        \
};                                                            \
DEVICE_DT_INST_DEFINE(inst,                                   \
                      charging_status_init,                   \
                      NULL,                                   \
                      &data_##inst,                           \
                      &cfg_##inst,                            \
                      POST_KERNEL,                            \
                      CONFIG_APPLICATION_INIT_PRIORITY,       \
                      NULL);

DT_INST_FOREACH_STATUS_OKAY(CHARGING_STATUS_DEFINE)