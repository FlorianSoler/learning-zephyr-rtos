#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
static uint8_t byte;

/* GPIO definition for P0.13 */
#define LED_NODE DT_NODELABEL(gpio0)
static const struct device *gpio0_dev = DEVICE_DT_GET(LED_NODE);

void uart_cb(const struct device *dev, void *user_data)
{
    while (uart_fifo_read(dev, &byte, 1)) {
        printk("%c", byte);   // print GPS data
    }
}

void main(void)
{
    if (!device_is_ready(uart)) {
        return;
    }

    /* --- GPIO setup (P0.13 HIGH) --- */
    if (device_is_ready(gpio0_dev)) {
        gpio_pin_configure(gpio0_dev, 13, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set(gpio0_dev, 13, 1);  // HIGH
    }

    /* --- UART setup --- */
    uart_irq_callback_user_data_set(uart, uart_cb, NULL);
    uart_irq_rx_enable(uart);

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}