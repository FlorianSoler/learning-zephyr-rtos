#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include "keyfob_service.h"

#define PAIR_BUTTON_NODE DT_PATH(buttons, button_0)
static const struct gpio_dt_spec pair_button = GPIO_DT_SPEC_GET(PAIR_BUTTON_NODE, gpios);

static int init_button(void)
{
    if (!gpio_is_ready_dt(&pair_button)) {
        printk("Erreur : Le GPIO du bouton n'est pas pret.\n");
        return -ENODEV;
    }
    return gpio_pin_configure_dt(&pair_button, GPIO_INPUT);
}

int main(void)
{
    int err;

    printk("Demarrage de la Centrale Bluetooth...\n");

    err = init_button();
    if (err) return err;

    err = keyfob_init();
    if (err) {
        printk("Echec de l'activation du module Keyfob (err %d)\n", err);
        return err;
    }

    printk("Scan actif global lance avec succes. En attente de la cible...\n");

    while (1) {
        // Look at the pin status matching your original inversion loop syntax
        if (gpio_pin_get_dt(&pair_button) == 1) {
            keyfob_allow_pairing();
        }

        if (keyfob_is_in_range()) {
            // Fob is near
        }

        k_msleep(100);
    }
    return 0;
}