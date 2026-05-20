#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* ── UART devices ───────────────────────────────────────────── */
static const struct device *uart0_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *uart1_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* ── GPIO P0.13 ─────────────────────────────────────────────── */
static const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* ── Line buffers ───────────────────────────────────────────── */
static char line0[256];
static char line1[256];
static int  idx0;
static int  idx1;

/* ── UART0 callback ─────────────────────────────────────────── */
static void uart0_cb(const struct device *dev, void *user_data)
{
    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }

    uint8_t buf[64];
    int len;

    while ((len = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < len; i++) {
            char c = buf[i];
            if (c == '\n' || idx0 >= sizeof(line0) - 1) {
                line0[idx0] = '\0';
                LOG_INF("[UART0] %s", line0);
                idx0 = 0;
            } else if (c != '\r') {
                line0[idx0++] = c;
            }
        }
    }
}

/* ── UART1 callback ─────────────────────────────────────────── */
static void uart1_cb(const struct device *dev, void *user_data)
{
    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }

    uint8_t buf[64];
    int len;

    while ((len = uart_fifo_read(dev, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < len; i++) {
            char c = buf[i];
            if (c == '\n' || idx1 >= sizeof(line1) - 1) {
                line1[idx1] = '\0';
                LOG_INF("[UART1] %s", line1);
                idx1 = 0;
            } else if (c != '\r') {
                line1[idx1++] = c;
            }
        }
    }
}

/* ── Main ───────────────────────────────────────────────────── */
int main(void)
{
    /* GPIO P0.13 HIGH */
    if (device_is_ready(gpio0_dev)) {
        gpio_pin_configure(gpio0_dev, 13, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set(gpio0_dev, 13, 1);
        LOG_INF("P0.13 HIGH");
    }

    /* UART0 */
    if (!device_is_ready(uart0_dev)) {
        LOG_ERR("UART0 not ready");
    } else {
        uart_irq_callback_user_data_set(uart0_dev, uart0_cb, NULL);
        uart_irq_rx_enable(uart0_dev);
        LOG_INF("UART0 ready (RX=P0.20 TX=P0.22)");
    }

    /* UART1 */
    if (!device_is_ready(uart1_dev)) {
        LOG_ERR("UART1 not ready");
    } else {
        uart_irq_callback_user_data_set(uart1_dev, uart1_cb, NULL);
        uart_irq_rx_enable(uart1_dev);
        LOG_INF("UART1 ready (RX=P1.00 TX=P0.24)");
    }

    while (1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}