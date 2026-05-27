#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include "modem_driver.h"
#include "certificates.h"
#include "app_config.h"

static modem_ctx_t sim7080_ctx;

int main(void)
{
    char resp[256];
    
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    printk("Initializing Cellular Tracker (Context-Based Architecture)...\n");

    if (modem_init(&sim7080_ctx, uart_dev) != 0) {
        printk("Hardware error: UART interface not ready or context binding failed.\n");
        return -1;
    }

    k_sleep(K_SECONDS(2));

    modem_send_command(&sim7080_ctx, "ATE0", resp, sizeof(resp), 2000); 

    modem_send_command(&sim7080_ctx, "AT+CPIN?", resp, sizeof(resp), 3000);
    modem_send_command(&sim7080_ctx, "AT+CSQ", resp, sizeof(resp), 3000);
    modem_send_command(&sim7080_ctx, "AT+CEREG?", resp, sizeof(resp), 3000);
    modem_send_command(&sim7080_ctx, "AT+CGATT=1", resp, sizeof(resp), 10000);
    modem_send_command(&sim7080_ctx, "AT+CGDCONT=1,\"IP\",\"TM\"", resp, sizeof(resp), 5000);
    modem_send_command(&sim7080_ctx, "AT+CNACT?", resp, sizeof(resp), 5000);

    if (modem_provision_and_configure_tls(&sim7080_ctx, 0, 
                                          root_ca, root_ca_len, 
                                          client_cert, client_cert_len, 
                                          client_key, client_key_len) != 0) {
        printk("Boot halted: Secure TLS Provisioning Pipeline Failed.\n");
        return -1;
    }

    printk("\nSYSTEM CONFIGURATION COMPLETE\n");

    if (modem_mqtt_connect(&sim7080_ctx, 
                           MQTT_BROKER_URL, 
                           MQTT_BROKER_PORT, 
                           MQTT_CLIENT_ID, 
                           MQTT_KEEP_ALIVE_SEC, 
                           MQTT_CLEAN_SESSION, 
                           MQTT_SSL_CTX_INDEX) != 0) {
        printk("Network error: MQTT secure handshaking failed.\n");
        return -1;
    }

    int ret = modem_mqtt_publish(&sim7080_ctx, 
                                "tracker/" MQTT_CLIENT_ID "/msg", // Topic: tracker/client_4/msg
                                "hello world",                    // Payload
                                1,                                // QoS 1 (Acknowledged)
                                0);                               // Retain (0 = Do not retain)

    if (ret == 0) {
        printk("Payload successfully sent!\n");
    } else {
        printk("Error: Failed to publish message (Code: %d)\n", ret);
    }

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}