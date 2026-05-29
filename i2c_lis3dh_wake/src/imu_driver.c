#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include "imu_driver.h"

int lis3dh_init(imu_service_t *service)
{
    if (service == NULL) {
        return -EINVAL;
    }

    /* Locate the physical I2C device node defined in your device tree */
    const struct device *i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));

    if (!device_is_ready(i2c_bus)) {
        printk("Error: LIS3DH Driver could not bind to i2c0 device node.\n");
        return -ENODEV;
    }

    /* Bind the working bus handles straight into our application context tracking struct */
    service->i2c_port = i2c_bus;
    service->imu_address = LIS3DH_ADDR;

    printk("LIS3DH Driver successfully initialized on i2c0 bus interface.\n");
    return 0;
}

int lis3dh_configure_motion_interrupt(imu_service_t *service)
{
    int rc;

    #define WRITE_REG(reg, val) \
        rc = i2c_reg_write_byte(service->i2c_port, service->imu_address, reg, val); \
        if (rc < 0) { \
            printk("I2C write error at register 0x%X, code: %d\n", reg, rc); \
            return rc; \
        }

    /* 50Hz ODR, Low-power mode enabled, X/Y/Z axes execution turned on */
    WRITE_REG(LIS3DH_REG_CTRL1, 0b01011111); 
    
    /* High-pass filter enabled specifically routing to INT1 layout */
    WRITE_REG(LIS3DH_REG_CTRL2, 0x09);       
    
    /* INT1 IA1 event routed natively to physical INT1 pin */
    WRITE_REG(LIS3DH_REG_CTRL3, 0x40);       
    
    /* Block data update normal, ±4g scale selection */
    WRITE_REG(LIS3DH_REG_CTRL4, 0b00010000);       
    
    /* Latching off: Pin returns to LOW automatically as soon as motion stops */
    WRITE_REG(LIS3DH_REG_CTRL5, 0x00);       
    
    /* Interrupt active HIGH configuration setup */
    WRITE_REG(LIS3DH_REG_CTRL6, 0x00);       
    
    /* Interrupt threshold setting */
    WRITE_REG(LIS3DH_REG_INT1THS, 0b00000010);
    
    /* Minimum event duration filter step counter window */
    WRITE_REG(LIS3DH_REG_INT1DUR, 0x19);     
    
    /* Enable OR event detection logic mapping X, Y, Z high thresholds */
    WRITE_REG(LIS3DH_REG_INT1CFG, 0x2A);     

    #undef WRITE_REG
    return 0;
}