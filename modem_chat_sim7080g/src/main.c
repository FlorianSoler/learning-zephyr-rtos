#include <zephyr/kernel.h>
#include "modem_driver.h"
#include "certificates.h"

int main(void)
{
    char resp[256];

    printk("Initializing Cellular Tracker...\n");

    /* 1. Start Hardware */
    if (modem_hardware_init() != 0) {
        printk("Hardware error: UART interface not ready.\n");
        return -1;
    }

    k_sleep(K_SECONDS(2));

    /* 2. Configure Modem Basics */
    modem_send_command("ATE0", resp, sizeof(resp), 2000); 
    modem_send_command("AT+CPIN?", resp, sizeof(resp), 3000);
    modem_send_command("AT+CSQ", resp, sizeof(resp), 3000);
    modem_send_command("AT+CGATT=1", resp, sizeof(resp), 10000);

    /* 3. Flash Security Assets */
    if (modem_write_file(3, "root_ca.pem", root_ca, root_ca_len, 2) != 0) {
        printk("Boot halted: Root CA failed validation check.\n");
        return -1;
    }

    if (modem_write_file(3, "client_cert.pem", client_cert, client_cert_len, 1) != 0) {
        printk("Boot halted: Client Cert failed validation check.\n");
        return -1;
    }

    /* Note: If using your traditional SEC1 ECC key, remember to use Type 4 here! */
    if (modem_write_file(3, "client_key.pem", client_key, client_key_len, 4) != 0) {
        printk("Boot halted: Private Key failed validation check.\n");
        return -1;
    }

    printk("\nSYSTEM CONFIGURATION COMPLETE\n");
    
    while (1) {
        // Future MQTT logic goes here
        k_sleep(K_SECONDS(1));
    }
}