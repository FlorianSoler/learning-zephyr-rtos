/*
 * USB CDC log + UART0 + UART1
 * Board : promicro_nrf52840/nrf52840
 * Zephyr: 4.x  (+  -S cdc-acm-console snippet)
 *
 * - USB CDC  → printk / LOG output (via snippet)
 * - UART0    → TX P0.20 / RX P0.22 @ 115200
 * - UART1    → TX P0.24 / RX P1.00 @ 115200
 *
 * Every second the tick counter is printed to all three channels.
 * Any byte received on UART0 or UART1 is echoed back on the same port
 * and also printed to USB CDC.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_DBG);

/* ── Device handles ──────────────────────────────────────────────── */
static const struct device *uart0 = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *uart1 = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* ── Helper: write a string to a UART ───────────────────────────── */
static void uart_print(const struct device *dev, const char *str)
{
    while (*str) {
        uart_poll_out(dev, *str++);
    }
}

static void uart_printf(const struct device *dev, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintk(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uart_print(dev, buf);
}

/* ── IRQ callback: echo RX and forward to USB CDC ───────────────── */
static void uart_irq_handler(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    uart_irq_update(dev);

    while (uart_irq_rx_ready(dev)) {
        uint8_t byte;
        if (uart_fifo_read(dev, &byte, 1) > 0) {
            uart_poll_out(dev, byte);           /* echo on same UART */
            printk("%s RX: 0x%02x '%c'\r\n",   /* forward to USB CDC */
                   dev == uart0 ? "UART0" : "UART1",
                   byte, byte >= 0x20 ? byte : '.');
        }
        uart_irq_update(dev);
    }
}

/* ── main ────────────────────────────────────────────────────────── */
int main(void)
{
    /* Verify devices are ready */
    if (!device_is_ready(uart0)) {
        printk("ERROR: uart0 not ready\r\n");
        return -ENODEV;
    }
    if (!device_is_ready(uart1)) {
        printk("ERROR: uart1 not ready\r\n");
        return -ENODEV;
    }

    /* Register IRQ callbacks */
    uart_irq_callback_user_data_set(uart0, uart_irq_handler, NULL);
    uart_irq_rx_enable(uart0);

    uart_irq_callback_user_data_set(uart1, uart_irq_handler, NULL);
    uart_irq_rx_enable(uart1);

    /* Boot delay for USB CDC host enumeration */
    k_sleep(K_MSEC(2000));

    /* Banner on all three channels */
    printk("\r\n=== nRF52840 ready  USB-CDC + UART0(P0.20/22) + UART1(P0.24/P1.00) ===\r\n");
    uart_print(uart0, "\r\n=== UART0 ready (TX P0.20 / RX P0.22 / 115200) ===\r\n");
    uart_print(uart1, "\r\n=== UART1 ready (TX P0.24 / RX P1.00 / 115200) ===\r\n");

    uint32_t counter = 0;

    while (1) {
        uint32_t ms  = k_uptime_get_32();
        uint32_t sec = ms / 1000;
        uint32_t rem = ms % 1000;

        /* USB CDC */
        printk("[%6u.%03u] tick #%04u\r\n", sec, rem, counter);

        /* UART0 */
        uart_printf(uart0, "[%6u.%03u] UART0 tick #%04u\r\n",
                    sec, rem, counter);

        /* UART1 */
        uart_printf(uart1, "[%6u.%03u] UART1 tick #%04u\r\n",
                    sec, rem, counter);

        counter++;
        k_sleep(K_SECONDS(1));
    }

    return 0;
}