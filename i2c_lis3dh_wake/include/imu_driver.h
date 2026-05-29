#ifndef LIS3DH_IMU_H
#define LIS3DH_IMU_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>

/* LIS3DH I2C Address Options */
#define LIS3DH_ADDR          0x19

/* LIS3DH Register Layout Mapping */
#define LIS3DH_REG_CTRL1     0x20
#define LIS3DH_REG_CTRL2     0x21
#define LIS3DH_REG_CTRL3     0x22
#define LIS3DH_REG_CTRL4     0x23
#define LIS3DH_REG_CTRL5     0x24
#define LIS3DH_REG_CTRL6     0x25
#define LIS3DH_REG_INT1THS   0x32
#define LIS3DH_REG_INT1DUR   0x33
#define LIS3DH_REG_INT1CFG   0x30

/* Context structure containing the I2C bus reference */
typedef struct {
    const struct device *i2c_port;
    uint16_t imu_address;
} imu_service_t;

/**
 * @brief Find and initialize the I2C master bus, and link it to the service.
 * @param service Pointer to the service context structure to populate.
 * @return 0 on success, negative errno code on failure.
 */
int lis3dh_init(imu_service_t *service);

/**
 * @brief Configure LIS3DH interrupt engine for any-motion detection.
 * @param service Pointer to the instantiated tracking service structure.
 * @return 0 on success, negative errno code on failure.
 */
int lis3dh_configure_motion_interrupt(imu_service_t *service);

#endif /* LIS3DH_IMU_H */