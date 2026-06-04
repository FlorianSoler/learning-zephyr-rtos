#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include "keyfob_service.h"

// Fetch reset button settings from devicetree
#define RESET_BUTTON_NODE DT_ALIAS(reset_button)
#if !DT_NODE_HAS_STATUS_OKAY(RESET_BUTTON_NODE)
#define RESET_BUTTON_NODE DT_NODELABEL(reset_button)
#endif

static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);

/**
 * @brief Configures the physical hard-reset toggle pin.
 */
static int init_reset_button(void)
{
    if (!gpio_is_ready_dt(&reset_button)) {
        printk("Error: Hardware switch GPIO device is not ready.\n");
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&reset_button, GPIO_INPUT);
    if (ret != 0) return ret;

    return 0;
}

int main(void)
{
    int err;

    printk("Starting Keyfob Peripheral firmware application...\n");

    err = init_reset_button();
    if (err) return err;

    err = keyfob_init();
    if (err) return err;

    while (1) {
        // 1. Monitor the physical switch for factory resetting memory
        if (gpio_pin_get_dt(&reset_button) == 1) { 
            printk("\n[Switch] LOW level detected on pin! Running memory purge sequence...\n");

            keyfob_reset_pairing_memory();

            // Block loop execution while switch stays turned down to protect flash cycles
            while (gpio_pin_get_dt(&reset_button) == 1) {
                k_msleep(100); 
            }
            printk("[Switch] Returned to normal HIGH state. Ready.\n");
        }

        // 2. Clear, simple abstraction check to see if we are in range of the central
        if (keyfob_is_in_range()) {
            // Keep running normal keyfob presence routines...
        } else {
            // Keyfob is floating around unlinked/unpaired...
        }

        k_msleep(100); 
    }

    return 0;
}