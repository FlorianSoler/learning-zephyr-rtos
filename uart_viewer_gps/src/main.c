#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define LED_NODE DT_NODELABEL(gpio0)
#define NMEA_BUF_SIZE 128
#define GPS_STACK_SIZE 3072
#define GPS_PRIORITY 3

static const struct device *gpio0_dev = DEVICE_DT_GET(LED_NODE);
static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

/* Thread Data Struct Communication Wrapper */
struct nmea_frame {
    char line[NMEA_BUF_SIZE];
};

/* Define a Message Queue capable of holding 4 complete NMEA strings */
K_MSGQ_DEFINE(nmea_msgq, sizeof(struct nmea_frame), 4, 4);

/* Mutex protecting shared memory from simultaneous read/writes */
K_MUTEX_DEFINE(gps_data_mutex);

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

/* ===================================================================
 * NMEA Parser Helpers (Using Thread-Safe strtok_r and Float Literals)
 * =================================================================== */
static double nmea_to_decimal(const char *val, char dir)
{
    if (!val || strlen(val) == 0) return 0.0;
    
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
    if (!hhmmss || strlen(hhmmss) == 0) return 0;

    sscanf(hhmmss, "%2d%2d%f", &hh, &mm, &ss);
    return (hh * 3600) + (mm * 60) + (uint32_t)ss;
}

static void parse_rmc(char *line)
{
    char *token;
    char *saveptr; /* Required for reentrant strtok_r isolation */
    int field = 0;

    char time_str[16] = {0};
    char lat_str[16] = {0};
    char lon_str[16] = {0};
    char lat_dir = 0;
    char lon_dir = 0;
    float speed_knots = 0;

    token = strtok_r(line, ",", &saveptr);
    while (token) {
        switch (field) {
            case 1: strcpy(time_str, token); break;
            case 3: strcpy(lat_str, token);  break;
            case 4: lat_dir = token[0];       break;
            case 5: strcpy(lon_str, token);  break;
            case 6: lon_dir = token[0];       break;
            case 7: speed_knots = atof(token);break;
        }
        token = strtok_r(NULL, ",", &saveptr);
        field++;
    }

    gps.unix_time = parse_time(time_str);
    double lat = nmea_to_decimal(lat_str, lat_dir);
    double lon = nmea_to_decimal(lon_str, lon_dir);

    gps.lat_e7 = (int32_t)(lat * 1e7);
    gps.lon_e7 = (int32_t)(lon * 1e7);
    
    /* Fixed: Added 'f' suffixes to force native hardware single-precision float math */
    gps.speed_cmh = (uint16_t)(speed_knots * 1.852f * 100.0f);
}

static void parse_gga(char *line)
{
    char *token;
    char *saveptr;
    int field = 0;
    float hdop = 0;
    float alt = 0;

    token = strtok_r(line, ",", &saveptr);
    while (token) {
        switch (field) {
            case 8:  hdop = atof(token); break;
            case 9:  alt = atof(token);  break;
        }
        token = strtok_r(NULL, ",", &saveptr);
        field++;
    }

    gps.alt_m = (int16_t)alt;
    gps.hdop_x10 = (uint8_t)(hdop * 10.0f);
    
    /* Fixed: Changed 4.0 to 4.0f to prevent double promotion warnings */
    gps.accuracy_m = (uint8_t)(hdop * 4.0f);
}

static void process_nmea(char *line)
{
    if (strstr(line, "$GNRMC") || strstr(line, "$GPRMC")) {
        parse_rmc(line);
        printk("[PARSER] RMC sentence successfully evaluated\n");
    } else if (strstr(line, "$GNGGA") || strstr(line, "$GPGGA")) {
        parse_gga(line);
        printk("[PARSER] GGA updated -> LAT:%d | LON:%d | ALT:%dm\n", 
               gps.lat_e7, gps.lon_e7, gps.alt_m);
    }
}

/* ===================================================================
 * UART Interrupt Service Routine (Fast Execution Layer)
 * =================================================================== */
void uart_cb(const struct device *dev, void *user_data)
{
    static char isr_rx_buf[NMEA_BUF_SIZE];
    static int isr_rx_pos = 0;
    uint8_t c;

    /* Fixed: uart_irq_update returns void on this platform. Call it directly. */
    uart_irq_update(dev);

    if (uart_irq_rx_ready(dev)) {
        while (uart_fifo_read(dev, &c, 1)) {
            if (c == '\n' || c == '\r') {
                if (isr_rx_pos > 0) {
                    isr_rx_buf[isr_rx_pos] = '\0';
                    
                    /* Package sentence and drop it into the message queue safely */
                    struct nmea_frame frame;
                    memcpy(frame.line, isr_rx_buf, isr_rx_pos + 1);
                    
                    /* K_NO_WAIT guarantees the interrupt doesn't block */
                    k_msgq_put(&nmea_msgq, &frame, K_NO_WAIT);
                    isr_rx_pos = 0;
                }
            } else {
                if (isr_rx_pos < (NMEA_BUF_SIZE - 1)) {
                    isr_rx_buf[isr_rx_pos++] = c;
                }
            }
        }
    }
}

/* ===================================================================
 * Independent Parsing Worker Thread Context
 * =================================================================== */
void gps_processing_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    struct nmea_frame frame_data;

    printk("[SYSTEM] GPS Thread Processing Pipeline Active.\n");

    while (1) {
        /* Blocks passively here until the UART ISR puts data into the queue */
        if (k_msgq_get(&nmea_msgq, &frame_data, K_FOREVER) == 0) {
            
            /* Secure memory access using a mutex lock */
            k_mutex_lock(&gps_data_mutex, K_FOREVER);
            process_nmea(frame_data.line);
            k_mutex_unlock(&gps_data_mutex);
        }
    }
}

/* Statically register the thread structure */
K_THREAD_DEFINE(gps_thread_id, GPS_STACK_SIZE, gps_processing_thread, 
                NULL, NULL, NULL, GPS_PRIORITY, 0, 0);

/* ===================================================================
 * Application System Entry Thread
 * =================================================================== */
int main(void)
{
    if (!device_is_ready(uart)) {
        printk("Error: UART device interface not ready.\n");
        return -1;
    }

    if (device_is_ready(gpio0_dev)) {
        gpio_pin_configure(gpio0_dev, 13, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set(gpio0_dev, 13, 1);
    }

    /* Assign ISR configurations */
    uart_irq_callback_user_data_set(uart, uart_cb, NULL);
    uart_irq_rx_enable(uart);

    while (1) {
        k_sleep(K_SECONDS(1));
    }
    return 0;
}