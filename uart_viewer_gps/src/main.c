#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <stdio.h>
#include <string.h>

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

#define BUF_SIZE 128

static char line[BUF_SIZE];
static uint8_t idx = 0;

static char lat[16];
static char lon[16];
static char alt[16];
static char speed[16];

/* ---------------- GPIO P0.13 ---------------- */
#define GPIO0_NODE DT_NODELABEL(gpio0)
static const struct device *gpio0_dev = DEVICE_DT_GET(GPIO0_NODE);

/* ---------------- RMC parser ---------------- */
static void parse_rmc(char *l)
{
    char *p = strtok(l, ",");
    int field = 0;
    char status = 'V';

    while (p) {
        field++;

        if (field == 3) status = p[0];
        if (field == 4) strncpy(lat, p, sizeof(lat));
        if (field == 6) strncpy(lon, p, sizeof(lon));
        if (field == 8) strncpy(speed, p, sizeof(speed));

        p = strtok(NULL, ",");
    }

    if (status == 'A') {
        printk("LAT: %s\nLON: %s\nSPEED: %s knots\n", lat, lon, speed);
    }
}

/* ---------------- GGA parser ---------------- */
static void parse_gga(char *l)
{
    char *p = strtok(l, ",");
    int field = 0;

    while (p) {
        field++;

        if (field == 10) {
            strncpy(alt, p, sizeof(alt));
        }

        p = strtok(NULL, ",");
    }

    printk("ALT: %s m\n", alt);
}

/* ---------------- process line ---------------- */
static void process_line(char *l)
{
    if (strncmp(l, "$GNRMC", 6) == 0) {
        parse_rmc(l);
    }
    else if (strncmp(l, "$GNGGA", 6) == 0) {
        parse_gga(l);
    }
}

/* ---------------- UART callback ---------------- */
static void uart_cb(const struct device *dev, void *user_data)
{
    uint8_t c;

    while (uart_fifo_read(dev, &c, 1)) {

        if (c == '\n') {
            line[idx] = '\0';
            process_line(line);
            idx = 0;
        }
        else if (idx < BUF_SIZE - 1) {
            line[idx++] = c;
        }
    }
}

/* ---------------- MAIN ---------------- */
void main(void)
{
    /* --- GPIO P0.13 HIGH --- */
    if (device_is_ready(gpio0_dev)) {
        gpio_pin_configure(gpio0_dev, 13, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set(gpio0_dev, 13, 1); // HIGH
    }

    /* --- UART --- */
    if (!device_is_ready(uart)) {
        return;
    }

    uart_irq_callback_user_data_set(uart, uart_cb, NULL);
    uart_irq_rx_enable(uart);

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}