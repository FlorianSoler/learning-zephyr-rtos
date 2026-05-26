#ifndef MODEM_DRIVER_H
#define MODEM_DRIVER_H

#include <zephyr/device.h>
#include <stddef.h>

/* Initialize the UART hardware and interrupt queue */
int modem_hardware_init(void);

/* Send an AT command and wait for OK/ERROR */
int modem_send_command(const char *cmd, char *resp, size_t resp_size, int timeout_ms);

/* Upload a file to the SIM7080 file system */
int modem_write_file(int index, const char *filename, const uint8_t *data, size_t length, int cert_type);

#endif // MODEM_DRIVER_H