#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <errno.h>

#define UART_NODE DT_NODELABEL(uart1)
static const struct device *uart = DEVICE_DT_GET(UART_NODE);
static const struct gpio_dt_spec pwrkey = GPIO_DT_SPEC_GET(DT_NODELABEL(sim7080_pwrkey), gpios);

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
 * FLUSH UART RX BUFFER
 * ========================================================= */
static void uart_flush_rx(void)
{
    uint8_t c;
    while (uart_poll_in(uart, &c) == 0) {}
}

/* =========================================================
 * AT COMMAND HANDLER
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
 * WAIT FOR BOOT TOKEN IN RX STREAM
 * ========================================================= */
static bool wait_for_token(const char *token, int timeout_ms)
{
    char buf[256];
    size_t pos = 0;
    uint8_t c;
    int64_t end_time = k_uptime_get() + timeout_ms;

    memset(buf, 0, sizeof(buf));

    while (k_uptime_get() < end_time) {
        if (uart_poll_in(uart, &c) == 0) {
            if (pos < sizeof(buf) - 1) {
                buf[pos++] = c;
                buf[pos] = 0;
            }
            if (strstr(buf, token)) {
                return true;
            }
        }
    }
    return false;
}

/* =========================================================
 * ENSURE MODEM IS ON
 * ========================================================= */
static int modem_ensure_on(void)
{
    char resp[128];

    /* simple AT ping — no LTE check needed */
    if (SendCommand("AT", resp, sizeof(resp), 2000) == 0) {
        printk("Modem already ON\n");
        return 0;
    }

    printk("Modem not responding, pulsing PWRKEY...\n");

    if (!gpio_is_ready_dt(&pwrkey)) {
        printk("PWRKEY GPIO not ready\n");
        return -1;
    }

    gpio_pin_configure_dt(&pwrkey, GPIO_OUTPUT_INACTIVE);
    gpio_pin_set_dt(&pwrkey, 1);
    k_sleep(K_MSEC(1200));
    gpio_pin_set_dt(&pwrkey, 0);

    printk("Waiting for modem boot...\n");

    if (!wait_for_token("RDY", 15000))        { printk("Timeout: RDY\n");        return -1; }
    printk("Got RDY\n");
    if (!wait_for_token("+CFUN: 1", 5000))    { printk("Timeout: +CFUN\n");      return -1; }
    printk("Got +CFUN: 1\n");
    if (!wait_for_token("+CPIN: READY", 5000)){ printk("Timeout: +CPIN\n");      return -1; }
    printk("Got +CPIN: READY\n");
    if (!wait_for_token("SMS Ready", 5000))   { printk("Timeout: SMS Ready\n");  return -1; }
    printk("Modem boot complete\n");

    return 0;
}

/* =========================================================
 * MAIN
 * ========================================================= */
int main(void)
{
    char resp[512];

    if (!device_is_ready(uart)) {
        printk("UART not ready\n");
        return 0;
    }

    if (modem_ensure_on() != 0) {
        printk("Failed to start modem\n");
        return -1;
    }

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
    /* ---------------- QUERY PDP ---------------- */
    SendCommand("AT+CNACT?", resp, sizeof(resp), 5000);

    printk("\nDONE\n");
    while (1) {
        k_sleep(K_SECONDS(1));
    }
}