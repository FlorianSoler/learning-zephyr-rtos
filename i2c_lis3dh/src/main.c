#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/sensor/lis2dh.h>

/* LIS3DH Register Constants */
#define LIS3DH_ADDR        0x19
#define LIS3DH_REG_CTRL1   0x20
#define LIS3DH_REG_CTRL2   0x21
#define LIS3DH_REG_CTRL3   0x22
#define LIS3DH_REG_CTRL4   0x23
#define LIS3DH_REG_CTRL5   0x24
#define LIS3DH_REG_CTRL6   0x25
#define LIS3DH_REG_INT1THS 0x32
#define LIS3DH_REG_INT1DUR 0x33
#define LIS3DH_REG_INT1CFG 0x30

typedef struct {
    const struct device *i2c_port;
    uint16_t imu_address;
} imu_service_t;

int configure_interrupt1(imu_service_t *service)
{
    int rc;
    #define WRITE_REG(reg, val) \
        rc = i2c_reg_write_byte(service->i2c_port, service->imu_address, reg, val); \
        if (rc < 0) { printk("I2C write error at register 0x%X\n", reg); return rc; }

    WRITE_REG(LIS3DH_REG_CTRL1, 0b01011111); // 50Hz, lopower mode, X/Y/Z enabled
    WRITE_REG(LIS3DH_REG_CTRL2, 0x09);       // High-pass filter enabled for INT1
    WRITE_REG(LIS3DH_REG_CTRL3, 0x40);       // INT1 event routed to INT1 pin
    WRITE_REG(LIS3DH_REG_CTRL4, 0b00010000);       // ±4g scale
    
    /* CRITICAL: We turn OFF latching here (0x00) so the LIS3DH drops the pin 
       automatically as soon as motion stops! */
    WRITE_REG(LIS3DH_REG_CTRL5, 0x00);       
    
    WRITE_REG(LIS3DH_REG_CTRL6, 0x00);       // Active HIGH interrupt
    WRITE_REG(LIS3DH_REG_INT1THS, 0b00000010);
    WRITE_REG(LIS3DH_REG_INT1DUR, 0x19);     // Short duration
    WRITE_REG(LIS3DH_REG_INT1CFG, 0x2A);     // Motion on X, Y, Z high

    #undef WRITE_REG
    return 0;
}

int main(void)
{
    const struct device *global_i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    if (!device_is_ready(global_i2c_bus)) {
        printk("I2C master bus not ready.\n");
        return -1;
    }

    imu_service_t imu_service = {
        .i2c_port = global_i2c_bus,
        .imu_address = LIS3DH_ADDR
    };

    if (configure_interrupt1(&imu_service) < 0) {
        printk("Failed to run custom hardware configurations.\n");
        return -1;
    }

    const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio0_dev)) {
        printk("GPIO0 device port driver not ready.\n");
        return -1;
    }

    /* Configure P0.31 purely as a normal input pin */
    gpio_pin_configure(gpio0_dev, 31, GPIO_INPUT | GPIO_PULL_DOWN);

    printk("System active. Monitoring P0.31 via polling...\n");

    while (1) {
        /* Read physical pin state directly */
        int pin_state = gpio_pin_get(gpio0_dev, 31);

        if (pin_state > 0) {
            printk("Motion detected! Pin P0.31 is HIGH.\n");

            /* Freeze and wait loop: Do nothing while the hardware holds the pin high */
            while (gpio_pin_get(gpio0_dev, 31) > 0) {
                k_sleep(K_MSEC(50)); // Small sleep so the CPU isn't melting at 100% usage
            }

            printk("Motion stopped. Pin P0.31 is back to LOW.\n");
        }

        /* Ambient checking speed when idle */
        k_sleep(K_MSEC(100));
    }

    return 0;
}