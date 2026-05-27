#ifndef GPS_PROTOCOL_H
#define GPS_PROTOCOL_H

#include <zephyr/types.h>

/* Fast CPU Alignment Structure (No Padding Waste, Perfectly Sorted) */
typedef struct {
    uint32_t unix_time;      /* 4-byte aligned (True UTC Epoch) */
    int32_t lat_e7;          /* 4-byte aligned */
    int32_t lon_e7;          /* 4-byte aligned */
    int16_t alt_m;           /* 2-byte aligned */
    uint16_t speed_kph_x10;  /* 2-byte aligned */
    uint8_t hdop_x10;        /* 1-byte aligned */
    uint8_t reserved;        /* 1-byte padding to maintain 4-byte memory boundary */
} GPS_Point_Fast_t;

/* Packed Structure for Network Transmission (Exactly 17 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t unix_time;
    int32_t lat_e7;
    int32_t lon_e7;
    int16_t alt_m;
    uint16_t speed_kph_x10;
    uint8_t hdop_x10;
} GPS_Point_Packed_t;

#endif /* GPS_PROTOCOL_H */