/**
 * @file modem_driver.c
 * @brief SIM7080 modem driver implementation using Zephyr UART IRQ + message queue.
 *
 * Provides AT-command abstraction, TLS provisioning, MQTT connectivity,
 * and file system operations over UART-based modem interface.
 */
#ifndef MODEM_DRIVER_H
#define MODEM_DRIVER_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <stddef.h>
#include <stdint.h>

#define MODEM_RX_QUEUE_SIZE 1024

/** @brief Modem runtime instance context */
typedef struct {
    const struct device *uart_dev;              /* Tied hardware peripheral instance */
    struct k_msgq rx_msgq;                     /* Instance isolated message queue */
    uint8_t rx_queue_buffer[MODEM_RX_QUEUE_SIZE]; /* Memory backing for the queue */
    struct k_mutex lock;                        /* Protects the modem from multi-threaded overlap */
} modem_ctx_t;

/**
 * @brief Initialize modem driver instance.
 *
 * Configures UART, message queue, mutex, and interrupts.
 *
 * @param ctx Modem context instance
 * @param uart_device UART device pointer from device tree
 *
 * @return 0 on success, -ENODEV if UART not ready
 */
int modem_init(modem_ctx_t *ctx, const struct device *uart_device);

/**
 * @brief Send AT command and wait for response.
 *
 * Thread-safe function protected by mutex.
 *
 * @param ctx Modem context instance
 * @param cmd AT command string
 * @param resp Response buffer
 * @param resp_size Size of response buffer
 * @param timeout_ms Timeout in milliseconds
 *
 * @return 0 on success, -EIO or -ETIMEDOUT on failure
 */
int modem_send_command(modem_ctx_t *ctx, const char *cmd, char *resp, size_t resp_size, int timeout_ms);

/**
 * @brief Write file to modem filesystem.
 *
 * Splits data into chunks and streams via AT+CFSWFILE.
 *
 * @param ctx Modem context
 * @param index File storage index
 * @param filename Target filename on modem
 * @param data File data buffer
 * @param length Length of file data
 * @param cert_type Certificate type (root/client/key)
 *
 * @return 0 on success, negative error code on failure
 */
int modem_write_file(modem_ctx_t *ctx, int index, const char *filename, const uint8_t *data, size_t length, int cert_type);

/**
 * @brief Provision TLS credentials into modem.
 *
 * Uploads CA, client cert, and private key, then configures SSL context.
 *
 * @param ctx Modem context
 * @param ssl_idx SSL context index
 * @param root_ca Root CA certificate
 * @param root_ca_len Length of root CA
 * @param client_cert Client certificate
 * @param client_cert_len Length of client cert
 * @param client_key Private key
 * @param client_key_len Length of private key
 *
 * @return 0 on success, negative error code on failure
 */
int modem_provision_and_configure_tls(modem_ctx_t *ctx, int ssl_idx,
                                      const uint8_t *root_ca, size_t root_ca_len,
                                      const uint8_t *client_cert, size_t client_cert_len,
                                      const uint8_t *client_key, size_t client_key_len);

/**
 * @brief Connect modem to MQTT broker over TLS.
 *
 * Configures broker, client ID, keepalive, session, and SSL profile.
 *
 * @param ctx Modem context
 * @param url Broker URL
 * @param port Broker port
 * @param client_id MQTT client ID
 * @param keepalive Keepalive interval in seconds
 * @param clean_session Clean session flag
 * @param ssl_idx SSL context index
 *
 * @return 0 on success, negative error code on failure
 */
int modem_mqtt_connect(modem_ctx_t *ctx, const char *url, int port, 
                       const char *client_id, int keepalive, 
                       int clean_session, int ssl_idx);

/**
 * @brief Publish MQTT message.
 *
 * Sends payload using AT+SMPUB with QoS and retain flags.
 *
 * @param ctx Modem context
 * @param topic MQTT topic
 * @param payload Message payload
 * @param qos Quality of service level
 * @param retain Retain flag
 *
 * @return 0 on success, negative error code on failure
 */
int modem_mqtt_publish(modem_ctx_t *ctx, const char *topic, 
                       const char *payload, int qos, int retain);

/**
 * @brief Disconnect MQTT session.
 *
 * @param ctx Modem context
 *
 * @return 0 on success, negative error code on failure
 */
int modem_mqtt_disconnect(modem_ctx_t *ctx);

#endif