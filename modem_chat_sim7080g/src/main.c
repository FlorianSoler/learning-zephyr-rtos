#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <errno.h>

#define UART_NODE DT_NODELABEL(uart1)
static const struct device *uart = DEVICE_DT_GET(UART_NODE);
static const struct gpio_dt_spec pwrkey = GPIO_DT_SPEC_GET(DT_NODELABEL(sim7080_pwrkey), gpios);

static const unsigned char root_ca[] = {
    #include "root.cert.pem.inc"
};

// 2. Client Device Certificate (Automatically mapped from client_3.cert.pem)
static const unsigned char client_cert[] = {
    #include "client.cert.pem.inc"
};

// 3. Client Private Key (Automatically mapped from client_3.key.pem)
static const unsigned char client_key[] = {
    #include "client.key.pem.inc"
};

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
 * WAIT FOR RESPONSE TOKEN
 * ========================================================= */
static int wait_for_response(const char *token,
                             char *resp,
                             size_t resp_size,
                             int timeout_ms)
{
    memset(resp, 0, resp_size);

    int64_t end_time = k_uptime_get() + timeout_ms;
    size_t pos = 0;
    uint8_t c;

    while (k_uptime_get() < end_time) {

        if (uart_poll_in(uart, &c) == 0) {

            if (pos < resp_size - 1) {
                resp[pos++] = c;
                resp[pos] = 0;
            }

            if (strstr(resp, token)) {
                return 0;
            }

            if (strstr(resp, "ERROR")) {
                return -EIO;
            }
        }
    }

    return -ETIMEDOUT;
}

/* =========================================================
 * WRITE RAW DATA TO UART
 * ========================================================= */
static void uart_write_raw(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uart, data[i]);
    }
}

static int modem_write_file(int index,
                            const char *filename,
                            const uint8_t *data,
                            size_t length)
{
    static char resp[256];

    printk("\nWriting file: %s\n", filename);

    /* Attempt to initialize the filesystem */
    if (SendCommand("AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
        printk("CFSINIT failed. Workspace might be locked. Retrying cleanup...\n");
        
        /* Force terminate the stuck session */
        SendCommand("AT+CFSTERM", resp, sizeof(resp), 3000);
        
        /* Try initializing one more time */
        if (SendCommand("AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
            printk("Critical: CFSINIT failed twice. Cannot access filesystem.\n");
            return -1;
        }
    }

    /* Session successfully opened! Proceeding with data transfer... */
    size_t remaining = length;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = remaining > 10000 ? 10000 : remaining;
        char cmd[128];
        int mode = (offset == 0) ? 0 : 1;

        snprintf(cmd, sizeof(cmd), "AT+CFSWFILE=%d,\"%s\",%d,%u,10000",
                 index, filename, mode, (unsigned int)chunk);

        uart_flush_rx();
        printk("CMD: %s\n", cmd);
        uart_send(cmd);

        if (wait_for_response("DOWNLOAD", resp, sizeof(resp), 10000) != 0) {
            printk("No DOWNLOAD prompt\n");
            SendCommand("AT+CFSTERM", resp, sizeof(resp), 3000);
            return -1;
        }

        printk("Sending %u bytes...\n", (unsigned int)chunk);
        uart_write_raw(data + offset, chunk);

        if (wait_for_response("OK", resp, sizeof(resp), 30000) != 0) {
            printk("Write failed\n");
            SendCommand("AT+CFSTERM", resp, sizeof(resp), 3000);
            return -1;
        }

        offset += chunk;
        remaining -= chunk;
    }

    printk("File write complete\n");
    
    /* Always close the session when done */
    if (SendCommand("AT+CFSTERM", resp, sizeof(resp), 5000) != 0) {
        printk("CFSTERM failed\n");
        return -1;
    }

    return 0;
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
    printk("%s\n", "loaded certs");
    k_sleep(K_SECONDS(1));

    printk("CLIENT CERT\n%s\n\n CLIENT KEY\n%s", client_cert, client_key);
    k_sleep(K_SECONDS(1));

    printk("%s\n", "booting up");

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
    /* ---------------- IP PDP ---------------- */
    SendCommand("AT+CNACT=1,1", resp, sizeof(resp), 10000);
    /* ---------------- QUERY PDP ---------------- */
    SendCommand("AT+CNACT?", resp, sizeof(resp), 5000);

    // modem_write_file(
    //     3,                      // /customer/
    //     "root_ca.pem",
    //     (const uint8_t *)root_ca,
    //     strlen(root_ca)
    // );

    printk("\nDONE\n");
    while (1) {
        k_sleep(K_SECONDS(1));
    }
}