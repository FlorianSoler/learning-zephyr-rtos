#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <errno.h>

#define UART1_NODE DT_NODELABEL(uart1)
static const struct device *uart1_dev = DEVICE_DT_GET(UART1_NODE);
static const struct gpio_dt_spec pwrkey = GPIO_DT_SPEC_GET(DT_NODELABEL(sim7080_pwrkey), gpios);

/* Message Queue to pass characters from Hardware ISR to the App Layer */
K_MSGQ_DEFINE(uart1_rx_msgq, sizeof(uint8_t), 1024, 4); // Increased to 1024 to absorb bursts

static const unsigned char root_ca[] = {
    #include "root.cert.pem.inc"
};

static const unsigned char client_cert[] = {
    #include "client.cert.pem.inc"
};

static const unsigned char client_key[] = {
    #include "client.key.pem.inc"
};

/* =========================================================
 * HARDWARE INTERRUPT SERVICE ROUTINE (ISR)
 * ========================================================= */
static void uart_isr(const struct device *dev, void *user_data)
{
    /* Unused parameter tracking */
    ARG_UNUSED(user_data);

    /* Signal a status update state to the hardware block first */
    uart_irq_update(dev);

    /* Verify if data is waiting inside the hardware RX FIFO */
    if (uart_irq_rx_ready(dev)) {
        uint8_t c;
        /* Drain the FIFO bytes out entirely */
        while (uart_fifo_read(dev, &c, 1) == 1) {
            k_msgq_put(&uart1_rx_msgq, &c, K_NO_WAIT);
        }
    }
}

/* =========================================================
 * ASYNC-SAFE UTILITIES
 * ========================================================= */

static void uart_send(const struct device *dev, const char *cmd)
{
    for (int i = 0; cmd[i] != '\0'; i++) {
        uart_poll_out(dev, cmd[i]);
    }
    uart_poll_out(dev, '\r');
    uart_poll_out(dev, '\n');
}

static void uart_write_raw(const struct device *dev, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(dev, data[i]);
    }
}

static void uart_clear_buffers(void)
{
    uint8_t dummy;
    /* Flush out the RTOS message container */
    k_msgq_purge(&uart1_rx_msgq);
    /* Extra programmatic drain insurance */
    while (k_msgq_get(&uart1_rx_msgq, &dummy, K_NO_WAIT) == 0);
}

/* =========================================================
 * STREAM-BASED TOKEN MATCHER
 * ========================================================= */
static int wait_for_response(const char *token,
                             char *resp,
                             size_t resp_size,
                             int timeout_ms)
{
    if (resp && resp_size > 0) {
        memset(resp, 0, resp_size);
    }
    
    size_t pos = 0;
    uint8_t c;
    int64_t end_time = k_uptime_get() + timeout_ms;

    while (k_uptime_get() < end_time) {
        int64_t remaining = end_time - k_uptime_get();
        if (remaining <= 0) break;

        /* Sleep cleanly until the Hardware ISR feeds a byte into the queue */
        if (k_msgq_get(&uart1_rx_msgq, &c, K_MSEC(remaining)) == 0) {
            if (resp && pos < resp_size - 1) {
                resp[pos++] = c;
                resp[pos] = 0;
            }

            /* Check if our targeted keyword arrived in the sliding window stream */
            if (resp && strstr(resp, token)) {
                return 0;
            }
        }
    }
    return -ETIMEDOUT;
}

static int SendCommand(const struct device *dev,
                       const char *cmd,
                       char *resp,
                       size_t resp_size,
                       int timeout_ms)
{
    uart_clear_buffers();
    printk("\nSending: %s\n", cmd);
    uart_send(dev, cmd);

    int ret = wait_for_response("OK", resp, resp_size, timeout_ms);
    if (ret != 0) {
        /* If it wasn't OK, check if it was an explicit ERROR flag */
        if (resp && strstr(resp, "ERROR")) {
            return -EIO;
        }
    }
    return ret;
}

/* =========================================================
 * MODEM FILE SYSTEM OPERATIONS
 * ========================================================= */
static int modem_write_file(const struct device *dev,
                            int index,
                            const char *filename,
                            const uint8_t *data,
                            size_t length,
                            int cert_type) /* 1 = Client Cert, 2 = Root CA, 3 = Client Key, 0 = Regular File */
{
    static char resp[256];

    printk("\nWriting file: %s (%d bytes)\n", filename, (int)length);

    if (SendCommand(dev, "AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
        printk("CFSINIT session locked. Sending termination reset...\n");
        SendCommand(dev, "AT+CFSTERM", resp, sizeof(resp), 3000);
        if (SendCommand(dev, "AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
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
        uart_send(dev, cmd);

        if (wait_for_response("DOWNLOAD", resp, sizeof(resp), 5000) != 0) {
            printk("Timeout waiting for DOWNLOAD prompt. Received:\n%s\n", resp);
            SendCommand(dev, "AT+CFSTERM", resp, sizeof(resp), 3000);
            return -1;
        }

        printk("Streaming raw data array payload...\n");
        uart_write_raw(dev, data + offset, chunk);

        if (wait_for_response("OK", resp, sizeof(resp), 15000) != 0) {
            printk("Flash allocation sync write failed.\n");
            SendCommand(dev, "AT+CFSTERM", resp, sizeof(resp), 3000);
            return -1;
        }

        offset += chunk;
        remaining -= chunk;
    }

    printk("File complete. Releasing workspace context.\n");
    SendCommand(dev, "AT+CFSTERM", resp, sizeof(resp), 5000);

    return 0;
}

/* =========================================================
 * APPLICATION ENTRY
 * ========================================================= */
int main(void)
{
    char resp[256];

    if (!device_is_ready(uart1_dev)) {
        printk("Hardware error: UART interface not ready.\n");
        return 0;
    }

    /* -------------------------------------------------------------
     * REGISTER HARDWARE INTERRUPTS
     * ------------------------------------------------------------- */
    uart_irq_callback_set(uart1_dev, uart_isr);
    uart_irq_rx_enable(uart1_dev);
    printk("Hardware UART Interrupt Engine Active.\n");

    /* Wake up modem logic placeholder here if needed... */
    k_sleep(K_SECONDS(2));

    /* -------------------------------------------------------------
     * TURN OFF COMMAND ECHOING PERMANENTLY
     * ------------------------------------------------------------- */
    printk("Configuring Modem Environment...\n");
    SendCommand(uart1_dev, "ATE0", resp, sizeof(resp), 2000); 

    /* -------------------------------------------------------------
     * RUN REGULAR AT INITIALIZATION STACK
     * ------------------------------------------------------------- */
    SendCommand(uart1_dev, "AT+CPIN?", resp, sizeof(resp), 3000);
    SendCommand(uart1_dev, "AT+CSQ", resp, sizeof(resp), 3000);
    SendCommand(uart1_dev, "AT+CGATT=1", resp, sizeof(resp), 10000);

    /* -------------------------------------------------------------
     * FILE UPLOAD WITH AUTOMATIC CRYPTO CHECKS
     * ------------------------------------------------------------- */
    
    // Type 2 = Root CA
    if (modem_write_file(uart1_dev, 3, "root_ca.pem", (const uint8_t *)root_ca, sizeof(root_ca)- 1, 2) != 0) {
        printk("Boot halted: Root CA failed validation check.\n");
        return -1;
    }

    // Type 1 = Client Certificate
    if (modem_write_file(uart1_dev, 3, "client_cert.pem", (const uint8_t *)client_cert, sizeof(client_cert)- 1, 1) != 0) {
        printk("Boot halted: Client Cert failed validation check.\n");
        return -1;
    }

    // Type 3 = Client Private Key
    if (modem_write_file(uart1_dev, 3, "client_key.pem", (const uint8_t *)client_key, sizeof(client_key)- 1, 3) != 0) {
        printk("Boot halted: Private Key failed validation check.\n");
        return -1;
    }

    printk("\nSYSTEM CONFIGURATION COMPLETE\n");
    while (1) {
        k_sleep(K_SECONDS(1));
    }
}