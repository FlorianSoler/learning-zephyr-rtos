/* main.c */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/settings/settings.h>

#include <zephyr/drivers/gpio.h>

/* ---------------- Reset button ---------------- */

#define RESET_PIN 31

static const struct device *gpio_dev;
static struct gpio_callback reset_cb_data;

static bool bt_ready = false;

/* Forward declaration */
static void clear_all_bonds(void);

static void reset_button_pressed(const struct device *dev,
                                 struct gpio_callback *cb,
                                 uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    if (!bt_ready) {
        printk("BT not ready yet\n");
        return;
    }

    printk("Reset button pressed -> clearing BLE bonds\n");

    clear_all_bonds();
}

/* ---------------- Bond deletion ---------------- */

static void clear_bond_cb(const struct bt_bond_info *info,
                          void *user_data)
{
    ARG_UNUSED(user_data);
    bt_unpair(BT_ID_DEFAULT, &info->addr);
}

static void clear_all_bonds(void)
{
    bt_foreach_bond(BT_ID_DEFAULT,
                    clear_bond_cb,
                    NULL);
}

/* ---------------- BLE callbacks ---------------- */

static void start_adv(void)
{
    printk("Advertising started\n");
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .recycled = start_adv,
};

/* ---------------- MAIN ---------------- */

int main(void)
{
    int err;

    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (%d)\n", err);
        return 0;
    }

    /* 🔴 IMPORTANT FIX FOR BOND RESTORE */
    settings_subsys_init();
    settings_load();

    bt_ready = true;

    printk("Bluetooth initialized\n");

    /* ---------------- GPIO ---------------- */

    gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

    if (!device_is_ready(gpio_dev)) {
        printk("GPIO not ready\n");
        return 0;
    }

    err = gpio_pin_configure(gpio_dev,
                             RESET_PIN,
                             GPIO_INPUT | GPIO_PULL_UP);

    if (err) {
        printk("GPIO config failed\n");
        return 0;
    }

    err = gpio_pin_interrupt_configure(gpio_dev,
                                       RESET_PIN,
                                       GPIO_INT_EDGE_FALLING);

    if (err) {
        printk("Interrupt config failed\n");
        return 0;
    }

    gpio_init_callback(&reset_cb_data,
                       reset_button_pressed,
                       BIT(RESET_PIN));

    gpio_add_callback(gpio_dev, &reset_cb_data);

    printk("Reset button ready on P0.31\n");

    start_adv();

    return 0;
}