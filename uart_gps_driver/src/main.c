#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gnss.h>
#include <stdio.h>
#include <stdlib.h>

/* Get device reference from the devicetree overlay */
static const struct device *gnss_dev = DEVICE_DT_GET(DT_NODELABEL(gnss0));

/* ===================================================================
 * GNSS Data Event Callback
 * =================================================================== */
static void gnss_data_cb(const struct device *dev, const struct gnss_data *data)
{
    /* Check if the u-blox module has acquired a valid satellite lock */
    if (data->info.fix_status < GNSS_FIX_STATUS_GNSS_FIX) {
        printf("[GNSS] Searching for satellites... (No Fix)\n");
        return;
    }

    /* Zephyr stores Latitude and Longitude as 64-bit integers scaled by 10^9 
     * (nano-degrees) to avoid floating point drift or emulation overhead.
     * We divide to print them cleanly as "Integer.Fractional" parts.
     */
    long long lat_deg = data->nav_data.latitude / 1000000000LL;
    long long lat_frac = labs(data->nav_data.latitude % 1000000000LL);

    long long lon_deg = data->nav_data.longitude / 1000000000LL;
    long long lon_frac = labs(data->nav_data.longitude % 1000000000LL);

    /* Altitude is stored in millimeters. Divide by 1000 to get meters */
    int32_t alt_m = data->nav_data.altitude / 1000;
    int32_t alt_mm_frac = abs(data->nav_data.altitude % 1000);

    printf("\n================= U-BLOX M10 DATA =================");
    printf("\n  SATELLITES IN USE : %u", data->info.satellites_cnt);
    printf("\n  LATITUDE          : %lld.%09lld deg", lat_deg, lat_frac);
    printf("\n  LONGITUDE         : %lld.%09lld deg", lon_deg, lon_frac);
    printf("\n  ALTITUDE          : %d.%03d meters", alt_m, alt_mm_frac);
    printf("\n===================================================\n");
}

/* * FIXED: Pass DEVICE_DT_GET directly here instead of the 'gnss_dev' variable.
 * This supplies a true compile-time linker address to satisfy the C compiler.
 */
GNSS_DATA_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(gnss0)), gnss_data_cb);

/* ===================================================================
 * Main Entry Point
 * =================================================================== */
int main(void)
{
    printf("\nInitializing Subsystem Application Layer...\n");

    if (!device_is_ready(gnss_dev)) {
        printf("Error: GNSS driver device interface not ready!\n");
        return -1;
    }

    printf("GNSS Device Driver Active. Listening for incoming NMEA streams.\n");

    while (1) {
        /* The main thread does nothing but sleep; callbacks handle data */
        k_sleep(K_FOREVER);
    }

    return 0;
}