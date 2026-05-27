#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Ring Buffer Configurations */
#define BUFFER_MAX_POINTS     32   /* Total capacity of the internal circular queue */
#define BATCH_SEND_THRESHOLD  10   /* Configurable number of points needed to trigger a send */

/* Protocol Framing Constants */
#define PACKET_MAGIC_BYTE     0xAA /* Unique prefix byte to identify valid telemetry packets */

/* Thread Execution Parameters */
#define TX_THREAD_STACK_SIZE  2048
#define TX_THREAD_PRIORITY    7    /* Cooperative priority level */

#endif /* APP_CONFIG_H */