#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/poweroff.h>
#include <errno.h>
#include "imu_driver.h"

static const struct gpio_dt_spec int1_gpio = GPIO_DT_SPEC_GET(
    DT_CHILD(DT_NODELABEL(i2c0), lis3dh_19), irq_gpios);
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec pullup_pwr = GPIO_DT_SPEC_GET(
    DT_PATH(zephyr_user), pullup_power_gpios);

int main(void)
{
    int rc;
    bool imu_success = true;
    imu_service_t imu_service;

    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

    /* 1. Initialize LED0 */
    if (!gpio_is_ready_dt(&led0)) {
        return -ENODEV;
    }
    rc = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) return rc;

    if (!gpio_is_ready_dt(&pullup_pwr)) {
        return -ENODEV;
    }
    rc = gpio_pin_configure_dt(&pullup_pwr, GPIO_OUTPUT_ACTIVE);
    if (rc < 0) return rc;

    /* 2. Check IRQ GPIO ready */
    if (!gpio_is_ready_dt(&int1_gpio)) {
        imu_success = false;
    }

    /* 3. WAKE I2C before any transaction
     *    With PM_DEVICE_RUNTIME=y the bus starts suspended —
     *    pm_device_runtime_get() resumes it and increments the
     *    reference count so it stays up until we release it. */
    if (imu_success) {
        rc = pm_device_runtime_get(i2c_dev);
        if (rc < 0) {
            printk("Failed to resume I2C: %d\n", rc);
            imu_success = false;
        }
    }

    /* 4. Initialize and configure LIS3DH — bus is now awake */
    if (imu_success) {
        rc = lis3dh_init(&imu_service);
        if (rc < 0) {
            printk("Failed to initialize LIS3DH: %d\n", rc);
            imu_success = false;
        }
    }
    if (imu_success) {
        rc = lis3dh_configure_motion_interrupt(&imu_service);
        if (rc < 0) {
            printk("Failed to configure LIS3DH interrupts: %d\n", rc);
            imu_success = false;
        }
    }

    /* 5. Clear interrupt latch */
    if (imu_success) {
        uint8_t dummy;
        i2c_reg_read_byte(imu_service.i2c_port, imu_service.imu_address,
                          0x31, &dummy);
        k_msleep(10);
    }

    /* 6. RELEASE I2C — runtime PM will suspend it automatically
     *    This applies i2c0_sleep pinctrl (bias-disable + low-power-enable)
     *    and gates the TWIM peripheral clock. */
    pm_device_runtime_put(i2c_dev);
    printk("I2C released — peripheral now suspended by runtime PM.\n");

    /* 7. LED indication */
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

    gpio_pin_set_dt(&pullup_pwr, 0);

    /* 8. Configure wakeup pin */
    rc = gpio_pin_configure_dt(&int1_gpio, GPIO_INPUT);
    if (rc < 0) return rc;
    rc = gpio_pin_interrupt_configure_dt(&int1_gpio, GPIO_INT_LEVEL_ACTIVE);
    if (rc < 0) return rc;

    /* 9. System OFF */
    k_msleep(50);
    sys_poweroff();

    return 0;
}