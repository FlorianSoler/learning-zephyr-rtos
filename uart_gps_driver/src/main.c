#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/logging/log.h> /* Included Zephyr Logging module API */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "gps_protocol.h"

/* Register the logger instance module for this specific source file */
LOG_MODULE_REGISTER(gps_app, LOG_LEVEL_DBG);

/* Dynamic Ring Buffer Context */
typedef struct {
    GPS_Point_Fast_t data[BUFFER_MAX_POINTS];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    struct k_mutex lock;
} GPS_RingBuffer_t;

/* Static Initializations */
static GPS_RingBuffer_t gps_ring_buf;
K_SEM_DEFINE(sem_process_batch, 0, 1); 

static const struct device *gnss_dev = DEVICE_DT_GET(DT_NODELABEL(gnss0));

/* ===================================================================
 * RING BUFFER CORE LOGIC (ASYNC SAFE)
 * =================================================================== */
void ring_buffer_init(GPS_RingBuffer_t *buf)
{
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    k_mutex_init(&buf->lock);
}

void ring_buffer_push(GPS_RingBuffer_t *buf, const GPS_Point_Fast_t *point)
{
    k_mutex_lock(&buf->lock, K_FOREVER);

    buf->data[buf->head] = *point;
    buf->head = (buf->head + 1) % BUFFER_MAX_POINTS;

    if (buf->count < BUFFER_MAX_POINTS) {
        buf->count++;
    } else {
        buf->tail = (buf->tail + 1) % BUFFER_MAX_POINTS;
        LOG_WRN("Buffer Overflow Warning: Overwriting oldest stored GPS point!");
    }

    if (buf->count >= BATCH_SEND_THRESHOLD) {
        k_sem_give(&sem_process_batch);
    }

    k_mutex_unlock(&buf->lock);
}

int ring_buffer_pop_batch(GPS_RingBuffer_t *buf, GPS_Point_Fast_t *out_array, uint16_t requested_count)
{
    k_mutex_lock(&buf->lock, K_FOREVER);

    if (buf->count < requested_count) {
        k_mutex_unlock(&buf->lock);
        return -EAGAIN;
    }

    for (uint16_t i = 0; i < requested_count; i++) {
        out_array[i] = buf->data[buf->tail];
        buf->tail = (buf->tail + 1) % BUFFER_MAX_POINTS;
        buf->count--;
    }

    k_mutex_unlock(&buf->lock);
    return 0;
}

/* ===================================================================
 * ASYNCHRONOUS PACKING & TRANSMISSION TASK
 * =================================================================== */
void tx_processing_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    
    GPS_Point_Fast_t processing_space[BATCH_SEND_THRESHOLD];
    
    size_t header_sz = 2; 
    size_t packed_payload_sz = header_sz + (BATCH_SEND_THRESHOLD * sizeof(GPS_Point_Packed_t));
    uint8_t *tx_buffer = malloc(packed_payload_sz);

    LOG_INF("Async Transmission Worker Thread Initialized.");

    while (1) {
        k_sem_take(&sem_process_batch, K_FOREVER);

        if (ring_buffer_pop_batch(&gps_ring_buf, processing_space, BATCH_SEND_THRESHOLD) == 0) {
            
            if (!tx_buffer) {
                LOG_ERR("Dynamic heap allocation failed, cannot pack packet.");
                continue;
            }

            /* Write Custom Header [Magic Byte][Point Count] from app_config.h */
            tx_buffer[0] = PACKET_MAGIC_BYTE;
            tx_buffer[1] = (uint8_t)BATCH_SEND_THRESHOLD;

            /* Compress and pack data into payload buffer */
            uint8_t *payload_ptr = tx_buffer + header_sz;
            for (int i = 0; i < BATCH_SEND_THRESHOLD; i++) {
                GPS_Point_Packed_t packed_point = {
                    .unix_time       = processing_space[i].unix_time,
                    .lat_e7          = processing_space[i].lat_e7,
                    .lon_e7          = processing_space[i].lon_e7,
                    .alt_m           = processing_space[i].alt_m,
                    .speed_kph_x10   = processing_space[i].speed_kph_x10,
                    .hdop_x10        = processing_space[i].hdop_x10
                };
                
                memcpy(payload_ptr, &packed_point, sizeof(GPS_Point_Packed_t));
                payload_ptr += sizeof(GPS_Point_Packed_t);
            }

            /* Log Compiled Packet Metrics using standard logging macros */
            LOG_INF("Async Packet Compiled. Total Size: %u bytes", (unsigned int)packed_payload_sz);
            
            /* Zephyr's built-in hex-dump logging utility */
            LOG_HEXDUMP_INF(tx_buffer, packed_payload_sz, "Compiled Network Payload Hex:");

            /* Placeholder for MQTT publishing step... */
        }
    }
}

K_THREAD_DEFINE(tx_thread_id, TX_THREAD_STACK_SIZE, tx_processing_thread, 
                NULL, NULL, NULL, K_PRIO_COOP(TX_THREAD_PRIORITY), 0, 0);

/* ===================================================================
 * GNSS DATA EVENT CALLBACK
 * =================================================================== */
static void gnss_data_cb(const struct device *dev, const struct gnss_data *data)
{
    if (data->info.fix_status < GNSS_FIX_STATUS_GNSS_FIX) {
        LOG_DBG("Searching for satellites... (No Fix)");
        return;
    }

    struct tm tm_time = {
        .tm_sec  = (int)(data->utc.millisecond / 1000), 
        .tm_min  = data->utc.minute,
        .tm_hour = data->utc.hour,
        .tm_mday = data->utc.month_day,
        .tm_mon  = data->utc.month - 1,                 
        .tm_year = data->utc.century_year + 100,        
    };
    uint32_t real_unix_epoch = (uint32_t)timeutil_timegm(&tm_time);

    int32_t lat_scaled_e7 = (int32_t)(data->nav_data.latitude / 100LL);
    int32_t lon_scaled_e7 = (int32_t)(data->nav_data.longitude / 100LL);
    int16_t altitude_m    = (int16_t)(data->nav_data.altitude / 1000);
    uint16_t dynamic_speed_kph_x10 = (uint16_t)((data->nav_data.speed * 36) / 1000);
    uint8_t hdop_scaled_x10 = (uint8_t)(data->info.hdop / 100);

    GPS_Point_Fast_t current_point = {
        .unix_time     = real_unix_epoch,
        .lat_e7        = lat_scaled_e7,
        .lon_e7        = lon_scaled_e7,
        .alt_m         = altitude_m,
        .speed_kph_x10 = dynamic_speed_kph_x10,
        .hdop_x10      = hdop_scaled_x10
    };

    /* Standardized Debug Logging output statement */
    LOG_DBG("[GNSS Ingest] Lat: %d, Lon: %d, Alt: %dm, Speed: %u (x10), HDOP: %u (x10), UTC: %u", 
            current_point.lat_e7, current_point.lon_e7, current_point.alt_m, 
            current_point.speed_kph_x10, current_point.hdop_x10, current_point.unix_time);

    ring_buffer_push(&gps_ring_buf, &current_point);
}

GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gnss0)), gnss_data_cb);

/* ===================================================================
 * MAIN SYSTEM ENTRY POINT
 * =================================================================== */
int main(void)
{
    LOG_INF("Initializing Subsystem Application Layer...");
    ring_buffer_init(&gps_ring_buf);

    if (!device_is_ready(gnss_dev)) {
        LOG_ERR("GNSS driver device interface not ready!");
        return -1;
    }

    LOG_INF("GNSS Driver Active & Ring Buffer Running. Threshold: %d points.", BATCH_SEND_THRESHOLD);

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}