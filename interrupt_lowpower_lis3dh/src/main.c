#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(motion_app, LOG_LEVEL_INF);

/* Hardware Pin Definitions */
#define LIS3DH_ADDR         0x19
#define IMU_INT_PIN         31       /* P0.31 connected to LIS3DH INT1 pin */
#define WAIT_WINDOW_MS      5000     /* Time window to wait for 2nd pulse */

/* LIS3DH Registers */
#define LIS3DH_REG_CTRL1    0x20
#define LIS3DH_REG_CTRL2    0x21
#define LIS3DH_REG_CTRL3    0x22
#define LIS3DH_REG_CTRL4    0x23
#define LIS3DH_REG_CTRL5    0x24
#define LIS3DH_REG_CTRL6    0x25
#define LIS3DH_REG_INT1THS  0x32
#define LIS3DH_REG_INT1DUR  0x33
#define LIS3DH_REG_INT1CFG  0x30

static const struct device *gpio0_dev;
static const struct device *i2c_dev;
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec pullup_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), i2c_pullup_gpios);

static struct gpio_callback imu_cb;
static struct k_sem second_pulse_sem;
static struct k_work_delayable shutdown_work;

/* -------------------------------------------------------------------------- */
/* LIS3DH CONFIGURATION                                                       */
/* -------------------------------------------------------------------------- */
static int configure_lis3dh_motion_int(const struct device *i2c)
{
    int rc;
    #define WRITE_REG(reg, val) \
        rc = i2c_reg_write_byte(i2c, LIS3DH_ADDR, reg, val); \
        if (rc < 0) { LOG_ERR("I2C err reg 0x%02X", reg); return rc; }

    WRITE_REG(LIS3DH_REG_CTRL1, 0b01011111); // 50Hz, low-power mode, X/Y/Z active
    WRITE_REG(LIS3DH_REG_CTRL2, 0x09);       // High-pass filter enabled for INT1
    WRITE_REG(LIS3DH_REG_CTRL3, 0x40);       // Route INT1 event to INT1 physical pin
    WRITE_REG(LIS3DH_REG_CTRL4, 0b00010000); // ±4g scale
    WRITE_REG(LIS3DH_REG_CTRL5, 0x00);       // Non-latched interrupt (clears when motion stops)
    WRITE_REG(LIS3DH_REG_CTRL6, 0x00);       // Active HIGH interrupt output
    WRITE_REG(LIS3DH_REG_INT1THS, 0x02);     // Motion threshold
    WRITE_REG(LIS3DH_REG_INT1DUR, 0x02);     // Short duration filter
    WRITE_REG(LIS3DH_REG_INT1CFG, 0x2A);     // Motion on X, Y, or Z high

    #undef WRITE_REG
    return 0;
}

/* -------------------------------------------------------------------------- */
/* POWEROFF & SLEEP PREPARATION                                               */
/* -------------------------------------------------------------------------- */
static void enter_system_off(struct k_work *work)
{
    LOG_INF("Timeout reached with no 2nd pulse. Returning to System OFF...");

    if (device_is_ready(led0.port)) {
        gpio_pin_set_dt(&led0, 0);
    }

    /* Configure nRF Sense Pin: 
     * LIS3DH outputs Active HIGH on motion, so pull down P0.31 and 
     * configure sense for NRF_GPIO_PIN_SENSE_HIGH. */
    nrf_gpio_cfg_sense_input(IMU_INT_PIN, NRF_GPIO_PIN_PULLDOWN, NRF_GPIO_PIN_SENSE_HIGH);

    /* Flush logs before freezing clock */
    k_msleep(10);
    sys_poweroff();
}

/* -------------------------------------------------------------------------- */
/* INTERRUPT HANDLER (For 2nd Pulse Detection)                                 */
/* -------------------------------------------------------------------------- */
static void imu_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    /* Signal main thread that 2nd pulse arrived */
    k_sem_give(&second_pulse_sem);
}

/* -------------------------------------------------------------------------- */
/* APPLICATION LOGIC WORKFLOW                                                 */
/* -------------------------------------------------------------------------- */
static void execute_tracker_payload_logic(void)
{
    LOG_INF("========================================");
    LOG_INF("SECOND PULSE CONFIRMED! Executing task...");
    LOG_INF("========================================");

    /* Indicate action by turning on LED */
    if (device_is_ready(led0.port)) {
        gpio_pin_set_dt(&led0, 1);
    }

    /* Simulate modem TX, payload generation, or heavy processing */
    k_msleep(2000);

    if (device_is_ready(led0.port)) {
        gpio_pin_set_dt(&led0, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* MAIN ENTRY POINT                                                           */
/* -------------------------------------------------------------------------- */
int main(void)
{
    LOG_INF("System Booted / Woken From OFF Mode");

    gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    i2c_dev   = DEVICE_DT_GET(DT_NODELABEL(i2c0));

    int err = gpio_pin_configure_dt(&pullup_pin, GPIO_OUTPUT_ACTIVE);
    if (err) {
        LOG_ERR("Failed to configure pullup power pin: %d", err);
        return err;
    }

    if (!device_is_ready(gpio0_dev) || !device_is_ready(i2c_dev)) {
        LOG_ERR("Hardware peripherals not ready");
        return -ENODEV;
    }

    if (device_is_ready(led0.port)) {
        gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    }

    k_sem_init(&second_pulse_sem, 0, 1);
    k_work_init_delayable(&shutdown_work, enter_system_off);

    /* 1. Ensure LIS3DH motion engine remains armed */
    if (configure_lis3dh_motion_int(i2c_dev) < 0) {
        LOG_ERR("Failed to configure LIS3DH");
        return -EIO;
    }

    /* 2. Configure P0.31 as edge-triggered interrupt source for the second pulse */
    gpio_pin_configure(gpio0_dev, IMU_INT_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
    gpio_pin_interrupt_configure(gpio0_dev, IMU_INT_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&imu_cb, imu_gpio_isr, BIT(IMU_INT_PIN));
    gpio_add_callback(gpio0_dev, &imu_cb);

    LOG_INF("First pulse woke MCU. Listening for 2nd pulse (Window: %d ms)...", WAIT_WINDOW_MS);

    /* 3. Wait for 2nd pulse with a timeout */
    int rc = k_sem_take(&second_pulse_sem, K_MSEC(WAIT_WINDOW_MS));

    if (rc == 0) {
        /* Pulse received! Execute main active task */
        execute_tracker_payload_logic();
    } else {
        LOG_WRN("No 2nd pulse within timeframe (False Positive / Accidental bump).");
    }

    /* 4. Prepare for ultra-low power System OFF entry */
    k_work_reschedule(&shutdown_work, K_NO_WAIT);

    return 0;
}