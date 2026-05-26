#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "modem_driver.h"

#define UART1_NODE DT_NODELABEL(uart1)
static const struct device *uart1_dev = DEVICE_DT_GET(UART1_NODE);

K_MSGQ_DEFINE(uart1_rx_msgq, sizeof(uint8_t), 1024, 4);

/* --- PRIVATE HARDWARE FUNCTIONS --- */
static void uart_isr(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);
    uart_irq_update(dev);
    if (uart_irq_rx_ready(dev)) {
        uint8_t c;
        while (uart_fifo_read(dev, &c, 1) == 1) {
            k_msgq_put(&uart1_rx_msgq, &c, K_NO_WAIT);
        }
    }
}

static void uart_send(const char *cmd) {
    for (int i = 0; cmd[i] != '\0'; i++) uart_poll_out(uart1_dev, cmd[i]);
    uart_poll_out(uart1_dev, '\r');
    uart_poll_out(uart1_dev, '\n');
}

static void uart_write_raw(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) uart_poll_out(uart1_dev, data[i]);
}

static void uart_clear_buffers(void) {
    uint8_t dummy;
    k_msgq_purge(&uart1_rx_msgq);
    while (k_msgq_get(&uart1_rx_msgq, &dummy, K_NO_WAIT) == 0);
}

static int wait_for_response(const char *token, char *resp, size_t resp_size, int timeout_ms) {
    if (resp && resp_size > 0) memset(resp, 0, resp_size);
    size_t pos = 0;
    uint8_t c;
    int64_t end_time = k_uptime_get() + timeout_ms;

    while (k_uptime_get() < end_time) {
        int64_t remaining = end_time - k_uptime_get();
        if (remaining <= 0) break;

        if (k_msgq_get(&uart1_rx_msgq, &c, K_MSEC(remaining)) == 0) {
            if (resp && pos < resp_size - 1) {
                resp[pos++] = c;
                resp[pos] = 0;
            }
            if (resp && strstr(resp, token)) return 0;
        }
    }
    return -ETIMEDOUT;
}

/* --- PUBLIC DRIVER API --- */
int modem_hardware_init(void) {
    if (!device_is_ready(uart1_dev)) return -ENODEV;
    uart_irq_callback_set(uart1_dev, uart_isr);
    uart_irq_rx_enable(uart1_dev);
    return 0;
}

int modem_send_command(const char *cmd, char *resp, size_t resp_size, int timeout_ms) {
    uart_clear_buffers();
    printk("\nSending: %s\n", cmd);
    uart_send(cmd);

    int ret = wait_for_response("OK", resp, resp_size, timeout_ms);
    if (ret != 0 && resp && strstr(resp, "ERROR")) {
        return -EIO;
    }
    return ret;
}

int modem_write_file(int index, const char *filename, const uint8_t *data, size_t length, int cert_type)
{
    static char resp[256];

    printk("\nWriting file: %s (%d bytes)\n", filename, (int)length);

    // Replaced SendCommand with modem_send_command, and removed "dev"
    if (modem_send_command("AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
        printk("CFSINIT session locked. Sending termination reset...\n");
        modem_send_command("AT+CFSTERM", resp, sizeof(resp), 3000);
        if (modem_send_command("AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
            return -1;
        }
    }

    size_t remaining = length;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = remaining > 1024 ? 1024 : remaining; 
        char cmd[128];
        int mode = (offset == 0) ? 0 : 1;

        snprintf(cmd, sizeof(cmd), "AT+CFSWFILE=%d,\"%s\",%d,%u,10000",
                 index, filename, mode, (unsigned int)chunk);

        k_sleep(K_MSEC(30));
        uart_clear_buffers();

        printk("CMD: %s\n", cmd);
        uart_send(cmd); // Removed the "dev" argument

        if (wait_for_response("DOWNLOAD", resp, sizeof(resp), 5000) != 0) {
            printk("Timeout waiting for DOWNLOAD prompt. Received:\n%s\n", resp);
            modem_send_command("AT+CFSTERM", resp, sizeof(resp), 3000);
            return -1;
        }

        printk("Streaming raw data array payload...\n");
        uart_write_raw(data + offset, chunk); // Removed the "dev" argument

        if (wait_for_response("OK", resp, sizeof(resp), 15000) != 0) {
            printk("Flash allocation sync write failed.\n");
            modem_send_command("AT+CFSTERM", resp, sizeof(resp), 3000);
            return -1;
        }

        offset += chunk;
        remaining -= chunk;
    }

    printk("File complete. Releasing workspace context.\n");
    modem_send_command("AT+CFSTERM", resp, sizeof(resp), 5000);

    return 0;
}