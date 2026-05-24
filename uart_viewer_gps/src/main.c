#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <zephyr/drivers/gpio.h>

#define LED_NODE DT_NODELABEL(gpio0)

static const struct device *gpio0_dev =
    DEVICE_DT_GET(LED_NODE);

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

#define NMEA_BUF_SIZE 128

static char nmea_buf[NMEA_BUF_SIZE];
static int nmea_pos = 0;

typedef struct __attribute__((packed)) {

    uint32_t unix_time;

    int32_t lat_e7;
    int32_t lon_e7;

    int16_t alt_m;

    uint16_t speed_cmh;

    uint8_t hdop_x10;
    uint8_t accuracy_m;

} GPS_MQTT_Packet_t;

static GPS_MQTT_Packet_t gps;


///////////////////////////////////////////////////////////
// NMEA helpers
///////////////////////////////////////////////////////////

static double nmea_to_decimal(const char *val, char dir)
{
    double raw = atof(val);

    int deg = (int)(raw / 100);
    double minutes = raw - (deg * 100);

    double dec = deg + (minutes / 60.0);

    if (dir == 'S' || dir == 'W') {
        dec = -dec;
    }

    return dec;
}

static uint32_t parse_time(const char *hhmmss)
{
    int hh, mm;
    float ss;

    sscanf(hhmmss, "%2d%2d%f", &hh, &mm, &ss);

    // simple packed time for demo
    return (hh * 3600) + (mm * 60) + (uint32_t)ss;
}


///////////////////////////////////////////////////////////
// Parse RMC
///////////////////////////////////////////////////////////

static void parse_rmc(char *line)
{
    char *token;

    token = strtok(line, ",");

    int field = 0;

    char time_str[16] = {0};
    char lat_str[16] = {0};
    char lon_str[16] = {0};

    char lat_dir = 0;
    char lon_dir = 0;

    float speed_knots = 0;

    while (token) {

        switch (field) {

        case 1:
            strcpy(time_str, token);
            break;

        case 3:
            strcpy(lat_str, token);
            break;

        case 4:
            lat_dir = token[0];
            break;

        case 5:
            strcpy(lon_str, token);
            break;

        case 6:
            lon_dir = token[0];
            break;

        case 7:
            speed_knots = atof(token);
            break;
        }

        token = strtok(NULL, ",");
        field++;
    }

    gps.unix_time = parse_time(time_str);

    double lat = nmea_to_decimal(lat_str, lat_dir);
    double lon = nmea_to_decimal(lon_str, lon_dir);

    gps.lat_e7 = (int32_t)(lat * 1e7);
    gps.lon_e7 = (int32_t)(lon * 1e7);

    // knots -> km/h
    gps.speed_cmh = (uint16_t)(speed_knots * 1.852 * 100);
}


///////////////////////////////////////////////////////////
// Parse GGA
///////////////////////////////////////////////////////////

static void parse_gga(char *line)
{
    char *token;

    token = strtok(line, ",");

    int field = 0;

    float hdop = 0;
    float alt = 0;

    while (token) {

        switch (field) {

        case 8:
            hdop = atof(token);
            break;

        case 9:
            alt = atof(token);
            break;
        }

        token = strtok(NULL, ",");
        field++;
    }

    gps.alt_m = (int16_t)alt;

    gps.hdop_x10 = (uint8_t)(hdop * 10);

    // rough estimate
    gps.accuracy_m = (uint8_t)(hdop * 4.0);
}


///////////////////////////////////////////////////////////
// Handle full NMEA line
///////////////////////////////////////////////////////////

static void process_nmea(char *line)
{
    if (strstr(line, "$GNRMC") || strstr(line, "$GPRMC")) {

        parse_rmc(line);

        printk("RMC parsed\n");

    } else if (strstr(line, "$GNGGA") || strstr(line, "$GPGGA")) {

        parse_gga(line);

        printk("GGA parsed\n");

        printk("LAT: %d\n", gps.lat_e7);
        printk("LON: %d\n", gps.lon_e7);
        printk("SPD: %d\n", gps.speed_cmh);
        printk("ALT: %d\n", gps.alt_m);
        printk("HDOP: %d\n", gps.hdop_x10);
    }
}


///////////////////////////////////////////////////////////
// UART callback
///////////////////////////////////////////////////////////

void uart_cb(const struct device *dev, void *user_data)
{
    uint8_t c;

    while (uart_fifo_read(dev, &c, 1)) {

        if (c == '\n') {

            nmea_buf[nmea_pos] = 0;

            process_nmea(nmea_buf);

            nmea_pos = 0;

        } else {

            if (nmea_pos < (NMEA_BUF_SIZE - 1)) {
                nmea_buf[nmea_pos++] = c;
            }
        }
    }
}


///////////////////////////////////////////////////////////
// Main
///////////////////////////////////////////////////////////
void main(void)
{
    if (!device_is_ready(uart)) {
        return;
    }

    // Keep P0.13 HIGH
    if (device_is_ready(gpio0_dev)) {

        gpio_pin_configure(
            gpio0_dev,
            13,
            GPIO_OUTPUT_ACTIVE
        );

        gpio_pin_set(gpio0_dev, 13, 1);
    }

    uart_irq_callback_user_data_set(
        uart,
        uart_cb,
        NULL
    );

    uart_irq_rx_enable(uart);

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}