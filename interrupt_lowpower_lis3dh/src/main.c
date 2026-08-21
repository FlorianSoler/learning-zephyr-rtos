#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(motion_app, LOG_LEVEL_INF);

/* -------------------------------------------------------------------------- */
/* Hardware definitions (all sourced from the devicetree)                      */
/* -------------------------------------------------------------------------- */

static const struct device *const gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const i2c_dev   = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static const struct gpio_dt_spec led0 =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec pullup_pin =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), i2c_pullup_gpios);
static const struct gpio_dt_spec imu_int =
    GPIO_DT_SPEC_GET(DT_NODELABEL(lis3dh), irq_gpios);

#define LIS3DH_ADDR     DT_REG_ADDR(DT_NODELABEL(lis3dh))
#define WAIT_WINDOW_MS  5000  /* Time window to wait for the 2nd pulse */

/* -------------------------------------------------------------------------- */
/* LIS3DH motion-int register setup                                            */
/* -------------------------------------------------------------------------- */

#define LIS3DH_REG_CTRL1   0x20
#define LIS3DH_REG_CTRL2   0x21
#define LIS3DH_REG_CTRL3   0x22
#define LIS3DH_REG_CTRL4   0x23
#define LIS3DH_REG_CTRL5   0x24
#define LIS3DH_REG_CTRL6   0x25
#define LIS3DH_REG_INT1CFG 0x30
#define LIS3DH_REG_INT1THS 0x32
#define LIS3DH_REG_INT1DUR 0x33

static const struct {
    uint8_t reg;
    uint8_t value;
} lis3dh_config[] = {
    { LIS3DH_REG_CTRL1,   0b01011111 }, /* 50 Hz low-power mode, X/Y/Z active     */
    { LIS3DH_REG_CTRL2,   0x09 },       /* High-pass filter enabled for INT1      */
    { LIS3DH_REG_CTRL3,   0x40 },       /* Route INT1 event to INT1 physical pin  */
    { LIS3DH_REG_CTRL4,   0b00010000 }, /* +/-4g scale                            */
    { LIS3DH_REG_CTRL5,   0x00 },       /* Non-latched (clears when motion stops) */
    { LIS3DH_REG_CTRL6,   0x00 },       /* Active HIGH interrupt output           */
    { LIS3DH_REG_INT1THS, 0x02 },       /* Motion threshold                       */
    { LIS3DH_REG_INT1DUR, 0x02 },       /* Short duration filter                  */
    { LIS3DH_REG_INT1CFG, 0x2A },       /* Motion on X, Y or Z high               */
};

static int lis3dh_configure_motion_int(const struct device *i2c)
{
    for (size_t i = 0; i < ARRAY_SIZE(lis3dh_config); i++) {
        int rc = i2c_reg_write_byte(i2c, LIS3DH_ADDR,
                                    lis3dh_config[i].reg, lis3dh_config[i].value);
        if (rc < 0) {
            LOG_ERR("I2C write failed (reg 0x%02X, rc %d)", lis3dh_config[i].reg, rc);
            return rc;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* LED helpers                                                                 */
/* -------------------------------------------------------------------------- */

static void led_set(bool on)
{
    if (device_is_ready(led0.port)) {
        gpio_pin_set_dt(&led0, on);
    }
}

/* -------------------------------------------------------------------------- */
/* Device PM helpers                                                           */
/* -------------------------------------------------------------------------- */

static void device_set_power(const struct device *dev, enum pm_device_action action)
{
    int rc = pm_device_action_run(dev, action);

    if (rc < 0) {
        LOG_WRN("PM action %d on %s failed (rc %d)", action, dev->name, rc);
    }
}

/* -------------------------------------------------------------------------- */
/* Ultra-low power System OFF entry                                            */
/* -------------------------------------------------------------------------- */

static void prepare_system_off(void)
{
    LOG_INF("Preparing for ultra-low power System OFF entry...");
    led_set(false);

    /*
     * Re-arm the wake-up source before freezing the clock. The LIS3DH
     * outputs Active HIGH on motion, so pull its INT pin down and
     * configure the input to sense a rising edge.
     */
    nrf_gpio_cfg_sense_input(imu_int.pin, NRF_GPIO_PIN_PULLDOWN,
                             NRF_GPIO_PIN_SENSE_HIGH);

    /* Let the UART flush pending log lines before the clock stops. */
    k_msleep(10);
    
    sys_poweroff();
}

/* -------------------------------------------------------------------------- */
/* State shared between the ISR and main                                       */
/* -------------------------------------------------------------------------- */

static struct gpio_callback imu_cb;
static struct k_sem second_pulse_sem;

/* -------------------------------------------------------------------------- */
/* Interrupt handler for the 2nd pulse                                         */
/* -------------------------------------------------------------------------- */

static void imu_gpio_isr(const struct device *dev, struct gpio_callback *cb,
                         uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /* Signal the main thread that a 2nd pulse arrived. */
    k_sem_give(&second_pulse_sem);
}

/* -------------------------------------------------------------------------- */
/* Payload executed once the 2nd motion pulse is confirmed                     */
/* -------------------------------------------------------------------------- */

static void execute_tracker_payload_logic(void)
{
    LOG_INF("SECOND PULSE CONFIRMED! Executing task...");

    led_set(true);
    k_msleep(2000);  /* Simulate modem TX, payload generation, heavy processing */
    led_set(false);
}

/* -------------------------------------------------------------------------- */
/* Main                                                                        */
/* -------------------------------------------------------------------------- */

int main(void)
{
    LOG_INF("System Booted / Woken From OFF Mode");

    int err = gpio_pin_configure_dt(&pullup_pin, GPIO_OUTPUT_ACTIVE);
    if (err < 0) {
        LOG_ERR("Failed to configure pullup power pin: %d", err);
        return err;
    }

    if (!device_is_ready(gpio0_dev) || !device_is_ready(i2c_dev)) {
        LOG_ERR("Hardware peripherals not ready");
        return -ENODEV;
    }

    gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);

    k_sem_init(&second_pulse_sem, 0, 1);

    /* 1. Keep the LIS3DH motion engine armed */
    err = lis3dh_configure_motion_int(i2c_dev);
    if (err < 0) {
        LOG_ERR("Failed to configure LIS3DH (rc %d)", err);
        return -EIO;
    }

    /*
     * The I2C bus is no longer needed until the next sensor transaction, so
     * suspend the TWIM peripheral now. It stays off for the whole wait window
     * and only gets resumed right before any future i2c_*() call.
     */
    device_set_power(i2c_dev, PM_DEVICE_ACTION_SUSPEND);

    /* 2. Listen for the 2nd pulse on the INT pin (fallback wake source) */
    gpio_pin_configure_dt(&imu_int, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&imu_int, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&imu_cb, imu_gpio_isr, BIT(imu_int.pin));
    gpio_add_callback(imu_int.port, &imu_cb);

    LOG_INF("First pulse woke MCU. Listening for 2nd pulse (Window: %d ms)...",
            WAIT_WINDOW_MS);

    /* 3. Wait for the 2nd pulse within a window to discard false positives */
    if (k_sem_take(&second_pulse_sem, K_MSEC(WAIT_WINDOW_MS)) == 0) {
        execute_tracker_payload_logic();
    } else {
        LOG_WRN("No 2nd pulse within %d ms (false positive).", WAIT_WINDOW_MS);
    }

    /* 4. Enter ultra-low power System OFF */
    prepare_system_off();

    return 0;
}