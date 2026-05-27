#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "modem_driver.h"

LOG_MODULE_REGISTER(modem_driver, LOG_LEVEL_DBG);

/* =========================================================
 * HARDWARE INTERRUPT SERVICE ROUTINE (ISR)
 * ========================================================= */
static void uart_isr(const struct device *dev, void *user_data) 
{
    /* Cast user_data back to our specific modem context instance */
    modem_ctx_t *ctx = (modem_ctx_t *)user_data;
    if (!ctx) return;

    uart_irq_update(dev);

    if (uart_irq_rx_ready(dev)) {
        uint8_t c;
        while (uart_fifo_read(dev, &c, 1) == 1) {
            /* Feed the byte into this specific instance's queue */
            k_msgq_put(&ctx->rx_msgq, &c, K_NO_WAIT);
        }
    }
}

/* =========================================================
 * PRIVATE INSTANCE UTILITIES
 * ========================================================= */
static void uart_send(modem_ctx_t *ctx, const char *cmd) 
{
    for (int i = 0; cmd[i] != '\0'; i++) {
        uart_poll_out(ctx->uart_dev, cmd[i]);
    }
    uart_poll_out(ctx->uart_dev, '\r');
    uart_poll_out(ctx->uart_dev, '\n');
}

static void uart_write_raw(modem_ctx_t *ctx, const uint8_t *data, size_t len) 
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(ctx->uart_dev, data[i]);
    }
}

static void uart_clear_buffers(modem_ctx_t *ctx) 
{
    uint8_t dummy;
    k_msgq_purge(&ctx->rx_msgq);
    while (k_msgq_get(&ctx->rx_msgq, &dummy, K_NO_WAIT) == 0);

    LOG_DBG("RX buffer cleared");
}

static int wait_for_response(modem_ctx_t *ctx, const char *token, char *resp, size_t resp_size, int timeout_ms) 
{
    if (resp && resp_size > 0) memset(resp, 0, resp_size);
    size_t pos = 0;
    uint8_t c;
    int64_t end_time = k_uptime_get() + timeout_ms;

    while (k_uptime_get() < end_time) {
        int64_t remaining = end_time - k_uptime_get();
        if (remaining <= 0) break;

        /* Pop bytes out of our specific instance context queue */
        if (k_msgq_get(&ctx->rx_msgq, &c, K_MSEC(remaining)) == 0) {
            if (resp && pos < resp_size - 1) {
                resp[pos++] = c;
                resp[pos] = 0;
            }
            if (resp && strstr(resp, token)) {
                LOG_DBG("RX matched token: %s", token);
                return 0;
            }
        }
    }
    LOG_WRN("Timeout waiting for token: %s", token);
    return -ETIMEDOUT;
}

/* =========================================================
 * PUBLIC DRIVER API IMPLEMENTATION
 * ========================================================= */
int modem_init(modem_ctx_t *ctx, const struct device *uart_device) 
{
    if (!ctx || !device_is_ready(uart_device)) return -ENODEV;

    ctx->uart_dev = uart_device;

    /* Initialize core synchronization primitives dynamically inside the structure container */
    k_mutex_init(&ctx->lock);
    k_msgq_init(&ctx->rx_msgq, ctx->rx_queue_buffer, sizeof(uint8_t), MODEM_RX_QUEUE_SIZE);

    /* Bind the interrupt handler and securely pass the context address down as user data */
    uart_irq_callback_user_data_set(ctx->uart_dev, uart_isr, ctx);
    uart_irq_rx_enable(ctx->uart_dev);

    return 0;
}

int modem_send_command(modem_ctx_t *ctx,
                       const char *cmd,
                       char *resp,
                       size_t resp_size,
                       int timeout_ms)
{
    int ret;

    k_mutex_lock(&ctx->lock, K_FOREVER);

    uart_clear_buffers(ctx);
    uart_send(ctx, cmd);

    ret = wait_for_response(ctx, "OK", resp, resp_size, timeout_ms);

    if (ret != 0 && resp && strstr(resp, "ERROR")) {
        LOG_ERR("Modem returned ERROR: %s", resp);
        ret = -EIO;
    }

    k_mutex_unlock(&ctx->lock);
    return ret;
}

int modem_write_file(modem_ctx_t *ctx,
                     int index,
                     const char *filename,
                     const uint8_t *data,
                     size_t length,
                     int cert_type)
{
    ARG_UNUSED(cert_type);

    static char resp[256];

    k_mutex_lock(&ctx->lock, K_FOREVER);

    LOG_INF("Writing file %s (%u bytes)", filename, (unsigned int)length);

    if (modem_send_command(ctx, "AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {

        LOG_WRN("CFSINIT failed, attempting CFSTERM recovery");

        modem_send_command(ctx, "AT+CFSTERM", resp, sizeof(resp), 3000);

        if (modem_send_command(ctx, "AT+CFSINIT", resp, sizeof(resp), 5000) != 0) {
            LOG_ERR("CFSINIT recovery failed");
            k_mutex_unlock(&ctx->lock);
            return -EIO;
        }
    }

    size_t remaining = length;
    size_t offset = 0;

    while (remaining > 0) {

        size_t chunk = MIN(remaining, 1024);
        char cmd[128];
        int mode = (offset == 0) ? 0 : 1;

        snprintf(cmd, sizeof(cmd),
                 "AT+CFSWFILE=%d,\"%s\",%d,%u,10000",
                 index, filename, mode, (unsigned int)chunk);

        k_sleep(K_MSEC(30));
        uart_clear_buffers(ctx);

        uart_send(ctx, cmd);

        if (wait_for_response(ctx, "DOWNLOAD",
                               resp, sizeof(resp), 5000) != 0) {

            LOG_ERR("DOWNLOAD prompt timeout");
            modem_send_command(ctx, "AT+CFSTERM", resp, sizeof(resp), 3000);
            k_mutex_unlock(&ctx->lock);
            return -ETIMEDOUT;
        }

        uart_write_raw(ctx, data + offset, chunk);

        if (wait_for_response(ctx, "OK",
                               resp, sizeof(resp), 15000) != 0) {

            LOG_ERR("File write failed at offset %u", (unsigned int)offset);
            modem_send_command(ctx, "AT+CFSTERM", resp, sizeof(resp), 3000);
            k_mutex_unlock(&ctx->lock);
            return -EIO;
        }

        offset += chunk;
        remaining -= chunk;
    }

    LOG_INF("File upload complete");
    modem_send_command(ctx, "AT+CFSTERM", resp, sizeof(resp), 5000);

    k_mutex_unlock(&ctx->lock);
    return 0;
}

int modem_provision_and_configure_tls(modem_ctx_t *ctx, int ssl_idx,
                                      const uint8_t *root_ca, size_t root_ca_len,
                                      const uint8_t *client_cert, size_t client_cert_len,
                                      const uint8_t *client_key, size_t client_key_len)
{
    char cmd[128];
    char resp[128];
    int ret;

    LOG_INF("Provisioning TLS stack");

    /* Step 1: Upload the Root CA certificate to the modem's filesystem */
    ret = modem_write_file(ctx, 3, "ca.pem", root_ca, root_ca_len, 2);
    if (ret != 0) {
        LOG_ERR("Root CA provisioning failed");
        return ret;
    }

    /* Step 2: Upload the Client Certificate to the modem's filesystem */
    ret = modem_write_file(ctx, 3, "cert.pem", client_cert, client_cert_len, 1);
    if (ret != 0) {
        LOG_ERR("TLS Error: Failed to flash Client Certificate");
        return ret;
    }

    /* Step 3: Upload the Client Private Key to the modem's filesystem */
    ret = modem_write_file(ctx, 3, "key.pem", client_key, client_key_len, 4);
    if (ret != 0) {
        LOG_ERR("TLS Error: Failed to flash Client Private Key");
        return ret;
    }

    snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"sslversion\",%d,3", ssl_idx);
    ret = modem_send_command(ctx, cmd, resp, sizeof(resp), 2000);
    if (ret != 0) return ret;

    ret = modem_send_command(ctx, "AT+CSSLCFG=\"CONVERT\",2,\"root_ca.pem\"", resp, sizeof(resp), 2000);
    if (ret != 0) return ret;

    ret = modem_send_command(ctx, "AT+CSSLCFG=\"CONVERT\",1,\"client_cert.pem\",\"client_key.pem\"", resp, sizeof(resp), 2000);
    if (ret != 0) return ret;

    ret = modem_send_command(ctx, "AT+SMSSL=1,\"root_ca.pem\",\"client_cert.pem\"", resp, sizeof(resp), 2000);
    if (ret != 0) return ret;

    LOG_INF("TLS provisioning complete");
    return 0;
}

int modem_mqtt_connect(modem_ctx_t *ctx, const char *url, int port, 
                       const char *client_id, int keepalive, 
                       int clean_session, int ssl_idx)
{
    char cmd[128];
    char resp[128];
    int ret;

    LOG_INF("Starting MQTT connection setup");
    
    /* Lock the entire configuration bundle sequence */
    k_mutex_lock(&ctx->lock, K_FOREVER);

    /* =========================================================
     * STEP 0: Check if MQTT is already connected (+SMSTATE: 1)
     * ========================================================= */
    uart_clear_buffers(ctx);
    uart_send(ctx, "AT+SMSTATE?");
    
    /* Expect an 'OK' response, but we must parse the payload buffer */
    ret = wait_for_response(ctx, "OK", resp, sizeof(resp), 2000);
    if (ret == 0) {
        /* If the response contains "+SMSTATE: 1", the connection is alive */
        if (strstr(resp, "+SMSTATE: 1")) {
            LOG_INF("MQTT session is already open. Skipping reconnection.");
            ret = 0; 
            goto unlock_and_exit;
        }
        LOG_DBG("MQTT not connected (State response: %s). Proceeding.", resp);
    } else {
        LOG_WRN("Failed to read MQTT state, attempting normal connection flow.");
    }

    // 1. Set Broker URL and Port
    snprintf(cmd, sizeof(cmd), "AT+SMCONF=\"URL\",\"%s\",%d", url, port);
    ret = modem_send_command(ctx, cmd, resp, sizeof(resp), 2000);
    if (ret != 0) goto unlock_and_exit;

    // 2. Set Client ID
    snprintf(cmd, sizeof(cmd), "AT+SMCONF=\"CLIENTID\",\"%s\"", client_id);
    ret = modem_send_command(ctx, cmd, resp, sizeof(resp), 2000);
    if (ret != 0) goto unlock_and_exit;

    // 3. Set Keepalive Interval
    snprintf(cmd, sizeof(cmd), "AT+SMCONF=\"KEEPTIME\",%d", keepalive);
    ret = modem_send_command(ctx, cmd, resp, sizeof(resp), 2000);
    if (ret != 0) goto unlock_and_exit;

    // 4. Set Clean Session Flag
    snprintf(cmd, sizeof(cmd), "AT+SMCONF=\"CLEANSS\",%d", clean_session);
    ret = modem_send_command(ctx, cmd, resp, sizeof(resp), 2000);
    if (ret != 0) goto unlock_and_exit;

    // 5. Execute Connection Request (Allow up to 30 seconds for TLS Handshake)
    LOG_INF("Connecting to broker (TLS handshake)");
    
    /* Note: Increased timeout here slightly because your comment mentions 
       30 seconds, but your original code only gave it 3000ms (3 seconds). */
    ret = modem_send_command(ctx, "AT+SMCONN", resp, sizeof(resp), 30000);

unlock_and_exit:
    k_mutex_unlock(&ctx->lock);
    return ret;
}

int modem_mqtt_publish(modem_ctx_t *ctx, const char *topic, 
                       const char *payload, int qos, int retain)
{
    char cmd[128];
    char resp[128];
    size_t payload_len = strlen(payload);
    int ret;

    k_mutex_lock(&ctx->lock, K_FOREVER);

    // 1. Send the Publish request with exact payload length verification
    snprintf(cmd, sizeof(cmd), "AT+SMPUB=\"%s\",%u,%d,%d", 
             topic, (unsigned int)payload_len, qos, retain);
    
    uart_clear_buffers(ctx);
    LOG_DBG("MQTT publish topic=%s", topic);
    LOG_DBG("Payload len=%u", (unsigned)payload_len);
    
    // Explicit low-level write to handle step-by-step handshake
    for (int i = 0; cmd[i] != '\0'; i++) uart_poll_out(ctx->uart_dev, cmd[i]);
    uart_poll_out(ctx->uart_dev, '\r');
    uart_poll_out(ctx->uart_dev, '\n');

    // 2. Wait for the modem to present the data input prompt ('>')
    ret = wait_for_response(ctx, ">", resp, sizeof(resp), 5000);
    if (ret != 0) {
        LOG_ERR("Error: Modem did not present '>' data prompt.");
        goto publish_exit;
    }

    // 3. Stream raw payload text directly behind the prompt
    LOG_DBG("Streaming publication payload: %s", payload);
    for (size_t i = 0; i < payload_len; i++) {
        uart_poll_out(ctx->uart_dev, payload[i]);
    }

    // 4. Verify acknowledgment from the broker
    ret = wait_for_response(ctx, "OK", resp, sizeof(resp), 10000);
    if (ret != 0) {
        LOG_ERR("Error: Publication handshake acknowledgement failed.");
    }

publish_exit:
    k_mutex_unlock(&ctx->lock);
    return ret;
}

int modem_mqtt_disconnect(modem_ctx_t *ctx)
{
    char resp[64];
    return modem_send_command(ctx, "AT+SMDISC", resp, sizeof(resp), 5000);
}