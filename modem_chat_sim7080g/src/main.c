#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <errno.h>

#define UART_NODE DT_NODELABEL(uart1)

static const struct device *uart = DEVICE_DT_GET(UART_NODE);

/* =========================================================
 * UART SEND
 * ========================================================= */
static void uart_send(const char *cmd)
{
    for (int i = 0; cmd[i] != '\0'; i++) {
        uart_poll_out(uart, cmd[i]);
    }

    uart_poll_out(uart, '\r');
    uart_poll_out(uart, '\n');
}

/* =========================================================
 * FLUSH UART RX BUFFER (IMPORTANT FIX)
 * ========================================================= */
static void uart_flush_rx(void)
{
    uint8_t c;

    while (uart_poll_in(uart, &c) == 0) {
        /* discard old data */
    }
}

/* =========================================================
 * FIXED AT COMMAND HANDLER
 * - waits for OK
 * - waits for UART idle after OK
 * ========================================================= */
static int SendCommand(const char *cmd,
                       char *resp,
                       size_t resp_size,
                       int timeout_ms)
{
    memset(resp, 0, resp_size);
    uart_flush_rx();

    printk("\nSending: %s\n", cmd);
    uart_send(cmd);

    int64_t end_time = k_uptime_get() + timeout_ms;
    size_t pos = 0;
    uint8_t c;

    while (k_uptime_get() < end_time) {
        if (uart_poll_in(uart, &c) == 0) {
            if (pos < resp_size - 1) {
                resp[pos++] = c;
                resp[pos] = 0;
            }
            if (strstr(resp, "OK\r") || strstr(resp, "OK\n")) {
                printk("OK:\n%s\n", resp);
                return 0;
            }
            if (strstr(resp, "ERROR")) {
                printk("ERROR:\n%s\n", resp);
                return -EIO;
            }
        }
    }

    printk("TIMEOUT:\n%s\n", resp);
    return -ETIMEDOUT;
}

/* =========================================================
 * MAIN TEST FLOW (SIM7080)
 * ========================================================= */
int main(void)
{
    char resp[512];

    if (!device_is_ready(uart)) {
        printk("UART not ready\n");
        return 0;
    }

    printk("Waiting modem boot...\n");
    k_sleep(K_SECONDS(8));

    /* ---------------- BASIC ---------------- */
    SendCommand("AT", resp, sizeof(resp), 3000);

    /* ---------------- SIM ---------------- */
    SendCommand("AT+CPIN?", resp, sizeof(resp), 3000);

    /* ---------------- SIGNAL ---------------- */
    SendCommand("AT+CSQ", resp, sizeof(resp), 3000);

    /* ---------------- NETWORK ---------------- */
    SendCommand("AT+CEREG?", resp, sizeof(resp), 3000);

    /* ---------------- ATTACH ---------------- */
    SendCommand("AT+CGATT=1", resp, sizeof(resp), 10000);

    /* ---------------- APN ---------------- */
    SendCommand("AT+CGDCONT=1,\"IP\",\"TM\"", resp, sizeof(resp), 5000);

    /* ---------------- ACTIVATE PDP ---------------- */
    SendCommand("AT+CNACT=0,1", resp, sizeof(resp), 15000);

    /* ---------------- QUERY PDP ---------------- */
    SendCommand("AT+CNACT?", resp, sizeof(resp), 5000);

    printk("\nDONE\n");

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}