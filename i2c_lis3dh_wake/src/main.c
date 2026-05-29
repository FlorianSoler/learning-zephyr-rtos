#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/poweroff.h>
#include <errno.h>

#include "imu_driver.h"

/* Get the GPIO spec for the LIS3DH interrupt pin */
static const struct gpio_dt_spec int1_gpio = GPIO_DT_SPEC_GET(DT_CHILD(DT_NODELABEL(i2c0), lis3dh_19), irq_gpios);

/* Get the GPIO spec for the builtin LED0 via devicetree alias */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
    int rc;
    bool imu_success = true;
    imu_service_t imu_service;

    printk("\n--- nRF52840 Woke Up / Booted ---\n");

    /* 1. Initialize LED0 Hardware */
    if (!gpio_is_ready_dt(&led0)) {
        printk("Error: Builtin LED0 device is not ready\n");
        return -ENODEV;
    }

    rc = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        printk("Error configuring LED0 pin\n");
        return rc;
    }

    /* 2. Check if IMU GPIO hardware is ready */
    if (!gpio_is_ready_dt(&int1_gpio)) {
        printk("Error: GPIO device %s is not ready\n", int1_gpio.port->name);
        imu_success = false;
    }

    /* 3. Initialize and configure the LIS3DH */
    if (imu_success) {
        rc = lis3dh_init(&imu_service);
        if (rc < 0) {
            printk("Failed to initialize LIS3DH\n");
            imu_success = false;
        }
    }

    if (imu_success) {
        rc = lis3dh_configure_motion_interrupt(&imu_service);
        if (rc < 0) {
            printk("Failed to configure LIS3DH interrupts\n");
            imu_success = false;
        }
    }

    /* 4. Execute LED indication logic for 10 seconds */
    if (imu_success) {
        printk("IMU configured successfully! Blinking LED0 for 10 seconds...\n");
        
        // Blink pattern: 500ms ON, 500ms OFF for 10 cycles = 10 seconds
        for (int i = 0; i < 10; i++) {
            gpio_pin_set_dt(&led0, 1);
            k_msleep(500);
            gpio_pin_set_dt(&led0, 0);
            k_msleep(500);
        }
    } else {
        printk("IMU configuration failed! Leaving LED0 ON for 10 seconds...\n");
        
        // Solid pattern: Turn ON, hold for 10 seconds, turn OFF
        gpio_pin_set_dt(&led0, 1);
        k_msleep(10000);
        gpio_pin_set_dt(&led0, 0);
    }

    /* 5. Prepare GPIO for System OFF wakeup */
    printk("Configuring P0.31 for System OFF wake-up sense...\n");
    rc = gpio_pin_configure_dt(&int1_gpio, GPIO_INPUT);
    if (rc < 0) {
        printk("Error configuring GPIO input pin: %d\n", rc);
        return rc;
    }

    rc = gpio_pin_interrupt_configure_dt(&int1_gpio, GPIO_INT_LEVEL_ACTIVE);
    if (rc < 0) {
        printk("Error configuring GPIO interrupt sense: %d\n", rc);
        return rc;
    }

    /* 6. Go back to System OFF Deep Sleep */
    printk("Entering System OFF mode. Shake the board to wake me up!\n\n");
    k_msleep(100); // Small delay to clear UART logs cleanly

    sys_poweroff(); 

    return 0; // Execution never reaches here
}