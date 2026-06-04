#include "keyfob_service.h"

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/gpio.h>

#define TARGET_NAME "SC only peripheral"
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct bt_conn *default_conn = NULL;
static bool bond_found = false;
static bool allow_pairing = false;
static bool fob_in_range = false;

static struct k_work_delayable security_work;

static bool parse_device_name(struct bt_data *data, void *user_data)
{
    char *name = user_data;
    int len = MIN(data->data_len, 31); 

    switch (data->type) {
    case BT_DATA_NAME_SHORTENED:
    case BT_DATA_NAME_COMPLETE:
        memcpy(name, data->data, len);
        name[len] = '\0';
        return false; 
    default:
        return true;  
    }
}

static void check_bond_cb(const struct bt_bond_info *info, void *user_data)
{
    bool *found = user_data;
    *found = true;
    
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&info->addr, addr_str, sizeof(addr_str));
    printk("[Flash] Liaison existante trouvee avec : %s\n", addr_str);
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
    char name[32] = {0};
    int err;

    bt_data_parse(ad, parse_device_name, name);

    if (strcmp(name, TARGET_NAME) == 0) {
        if (bond_found || allow_pairing) {
            printk("[Centrale] CIBLE TROUVEE : '%s'. Arret du scan et connexion...\n", name);
            
            err = bt_le_scan_stop();
            if (err) return;

            err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN, 
                                    BT_LE_CONN_PARAM_DEFAULT, &default_conn);
            if (err) {
                bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
            }
        }
    }
}

static struct bt_le_scan_cb scan_callbacks = { .recv = scan_recv };

static void security_initiate_work(struct k_work *work)
{
    if (!default_conn) return;
    bt_conn_set_security(default_conn, BT_SECURITY_L4);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        if (default_conn == conn) {
            bt_conn_unref(default_conn);
            default_conn = NULL;
        }
        bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
        return;
    }

    gpio_pin_set_dt(&led0, 1);
    fob_in_range = true;
    k_work_schedule(&security_work, K_MSEC(50));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    gpio_pin_set_dt(&led0, 0);
    fob_in_range = false;
    k_work_cancel_delayable(&security_work);

    if (default_conn == conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }

    bond_found = false;
    bt_foreach_bond(BT_ID_DEFAULT, check_bond_cb, &bond_found);
    allow_pairing = false; 

    bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    if (!err) {
        bond_found = true; 
    } else {
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    bt_conn_auth_passkey_confirm(conn);
}

static struct bt_conn_auth_cb auth_cb_display = { .passkey_confirm = auth_passkey_confirm };

int keyfob_init(void)
{
    int err;

    if (!gpio_is_ready_dt(&led0)) return -ENODEV;
    gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);

    k_work_init_delayable(&security_work, security_initiate_work);

    err = bt_enable(NULL);
    if (err) return err;

    bt_conn_auth_cb_register(&auth_cb_display);

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

    bt_le_scan_cb_register(&scan_callbacks);

    struct bt_le_scan_param scan_param = {
        .type       = BT_LE_SCAN_TYPE_ACTIVE, 
        .options    = BT_LE_SCAN_OPT_NONE,    
        .interval   = BT_GAP_SCAN_FAST_INTERVAL,
        .window     = BT_GAP_SCAN_FAST_WINDOW,
    };

    return bt_le_scan_start(&scan_param, NULL);
}

bool keyfob_is_in_range(void)
{
    return fob_in_range;
}

// CRITICAL FIX: Ensure the button push forces alternative conditions open 
// exactly like your working sequential block.
void keyfob_allow_pairing(void)
{
    if (!bond_found) {
        if (!allow_pairing) {
            allow_pairing = true;
            printk("\n[Bouton] Mode appairage active ! Autorisation de se connecter...\n");
        }
    }
}