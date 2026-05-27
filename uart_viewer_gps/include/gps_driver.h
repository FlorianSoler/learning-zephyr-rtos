#ifndef GPS_DRIVER_H
#define GPS_DRIVER_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stdint.h>

#define GPS_NMEA_BUF_SIZE 128

/**
 * @brief Packed packet structure optimized for MQTT transmission
 */
typedef struct __attribute__((packed)) {
    uint32_t unix_time;
    int32_t lat_e7;
    int32_t lon_e7;
    int16_t alt_m;
    uint16_t speed_cmh;
    uint8_t hdop_x10;
    uint8_t accuracy_m;
} GPS_MQTT_Packet_t;

/**
 * @brief GPS Driver Context Instance Block
 */
typedef struct {
    const struct device *uart_dev;
    const struct device *pwr_gpio_dev;
    uint32_t pwr_pin;
    
    char nmea_buf[GPS_NMEA_BUF_SIZE];
    int nmea_pos;
    
    GPS_MQTT_Packet_t latest_data;
    struct k_mutex lock; // Ensures thread-safe data reads
} gps_ctx_t;

/**
 * @brief Initializes peripherals, applies power to GPS module, and hooks interrupts.
 * @return 0 on success, negative error code on failure.
 */
int gps_driver_init(gps_ctx_t *ctx, const struct device *uart_device, 
                    const struct device *gpio_device, uint32_t pwr_pin);

/**
 * @brief Thread-safe read access to copy the most recent valid GPS coordinates.
 */
void gps_driver_get_latest(gps_ctx_t *ctx, GPS_MQTT_Packet_t *dest);

#endif // GPS_DRIVER_H