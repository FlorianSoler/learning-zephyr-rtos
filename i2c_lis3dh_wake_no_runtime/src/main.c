#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/poweroff.h>
#include <errno.h>
#include <hal/nrf_twim.h>
#include "imu_driver.h"
#include <zephyr/pm/device.h>

static const struct gpio_dt_spec int1_gpio =
    GPIO_DT_SPEC_GET(DT_CHILD(DT_NODELABEL(i2c0), lis3dh_19), irq_gpios);
static const struct gpio_dt_spec led0 =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec pullup_pwr =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pullup_power_gpios);

/* ------------------------------------------------------------------
 * PINCTRL GENERATION FIX
 * ------------------------------------------------------------------ */
/* Force Zephyr's build system to compile the pinctrl structures for i2c0 
 * since we aren't using the standard runtime device driver initialization */
PINCTRL_DT_DEFINE(DT_NODELABEL(i2c0));

static const struct pinctrl_dev_config *i2c0_pcfg =
    PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c0));

int main(void)
{
    int rc;
    bool imu_success = true;
    imu_service_t imu_service;
    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

    /* ------------------------------------------------------------------
     * LED
     * ------------------------------------------------------------------ */
    if (!gpio_is_ready_dt(&led0)) {
        return -ENODEV;
    }
    rc = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) return rc;

    /* ------------------------------------------------------------------
     * POWER RAIL ON
     * ------------------------------------------------------------------ */
    if (!gpio_is_ready_dt(&pullup_pwr)) {
        return -ENODEV;
    }
    rc = gpio_pin_configure_dt(&pullup_pwr, GPIO_OUTPUT_ACTIVE);
    if (rc < 0) return rc;

    /* ------------------------------------------------------------------
     * DEVICE CHECK
     * ------------------------------------------------------------------ */
    if (!device_is_ready(i2c_dev)) {
        printk("I2C not ready\n");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&int1_gpio)) {
        imu_success = false;
    }

    /* ------------------------------------------------------------------
     * IMU INIT
     * ------------------------------------------------------------------ */
    if (imu_success) {
        rc = lis3dh_init(&imu_service);
        if (rc < 0) {
            printk("LIS3DH init failed: %d\n", rc);
            imu_success = false;
        }
    }

    if (imu_success) {
        rc = lis3dh_configure_motion_interrupt(&imu_service);
        if (rc < 0) {
            printk("LIS3DH interrupt config failed: %d\n", rc);
            imu_success = false;
        }
    }

    /* ------------------------------------------------------------------
     * CLEAR INTERRUPT LATCH
     * ------------------------------------------------------------------ */
    if (imu_success) {
        uint8_t dummy;
        i2c_reg_read_byte(imu_service.i2c_port,
                          imu_service.imu_address,
                          0x31, &dummy);
        k_msleep(10);
    }

    printk("IMU initialized: %s\n", imu_success ? "OK" : "FAIL");

    /* ------------------------------------------------------------------
     * LED INDICATION
     * ------------------------------------------------------------------ */
    if (imu_success) {
        for (int i = 0; i < 10; i++) {
            gpio_pin_set_dt(&led0, 1);
            k_msleep(500);
            gpio_pin_set_dt(&led0, 0);
            k_msleep(500);
        }
    } else {
        gpio_pin_set_dt(&led0, 1);
        k_msleep(10000);
        gpio_pin_set_dt(&led0, 0);
    }
    /* ------------------------------------------------------------------
     * SHUTDOWN SEQUENCE — target: minimal µA system OFF
     * ------------------------------------------------------------------ */

    /* Ensure LED is off */
    gpio_pin_set_dt(&led0, 0);

    /* Allow any pending I2C transaction to finish */
    k_msleep(10);

    /* 1. Turn off the peripheral */
    if (device_is_ready(i2c_dev)) {
        pm_device_action_run(i2c_dev, PM_DEVICE_ACTION_TURN_OFF);
    }

    /* 2. Turn off your custom pull-up hardware trace */
    gpio_pin_set_dt(&pullup_pwr, 0);

    /* 3. Give the silicon 500 microseconds to completely spin down its clocks */
    k_busy_wait(500);

    /* 4. Configure the wake-up interrupt pin */
    rc = gpio_pin_configure_dt(&int1_gpio, GPIO_INPUT);
    rc = gpio_pin_interrupt_configure_dt(&int1_gpio, GPIO_INT_LEVEL_HIGH);

    /* 5. Final 2ms window to let the level shifter voltages hit 0V */
    k_msleep(2);

    /* 6. Deep sleep */
    sys_poweroff();

    /* Never reached */
    return 0;
}