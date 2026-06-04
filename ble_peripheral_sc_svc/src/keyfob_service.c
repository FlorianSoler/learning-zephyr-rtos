#include "keyfob_service.h"

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/settings/settings.h>

static struct bt_conn *current_conn = NULL;
static bool connected_and_secured = false;

/* --- BLE Advertising Data Config --- */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_adv(void)
{
    int err;

    // Retry up to 3 times if buffers are temporarily locked during cleanup
    for (int retry = 0; retry < 3; retry++) {
        err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
        if (err == -ENOMEM) {
            k_msleep(50); // Yield to the BLE controller thread to free buffers
            continue;
        }
        break;
    }

    if (err) {
        printk("[Keyfob] Advertising failed to start (err %d)\n", err);
    } else {
        printk("[Keyfob] Advertising successfully started\n");
    }
}

/* --- Connection Callbacks --- */
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("[Keyfob] Failed to connect to %s (%u)\n", bt_conn_dst_str(conn), err);
        return;
    }

    printk("[Keyfob] Connected to %s\n", bt_conn_dst_str(conn));
    
    if (current_conn) {
        bt_conn_unref(current_conn);
    }
    current_conn = bt_conn_ref(conn);

    err = bt_conn_set_security(conn, BT_SECURITY_L4);
    if (err) {
        printk("[Keyfob] Failed to set security (err %d)\n", err);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("[Keyfob] Disconnected from %s, reason 0x%02x\n", bt_conn_dst_str(conn), reason);
    
    connected_and_secured = false;

    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    k_sleep(K_MSEC(100));
    start_adv();
}

static void identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa, const bt_addr_le_t *identity)
{
    printk("[Keyfob] Identity resolved %s -> %s\n", bt_addr_le_str(rpa), bt_addr_le_str(identity));
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    if (!err) {
        printk("[Keyfob] Security changed: %s level %u\n", bt_conn_dst_str(conn), level);
        if (level >= BT_SECURITY_L4) {
            connected_and_secured = true;
        }
    } else {
        printk("[Keyfob] Security failed: %s err %d\n", bt_conn_dst_str(conn), err);
        connected_and_secured = false;
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .identity_resolved = identity_resolved,
    .security_changed = security_changed,
};

/* --- Pairing and Auth Confirmation Handlers --- */
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    printk("[Keyfob] Passkey for %s: %06u\n", bt_conn_dst_str(conn), passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    printk("[Keyfob] Passkey code confirmation: %06u\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_cancel(struct bt_conn *conn)
{
    printk("[Keyfob] Pairing cancelled: %s\n", bt_conn_dst_str(conn));
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    printk("[Keyfob] Pairing Complete. Saved in flash: %s\n", bonded ? "YES" : "NO");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    printk("[Keyfob] Pairing Failed (%d). Disconnecting.\n", reason);
    bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .passkey_display = auth_passkey_display,
    .passkey_confirm = auth_passkey_confirm,
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_cb_info = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

/* --- Public API Implementations --- */

int keyfob_init(void)
{
    int err;

    bt_conn_auth_cb_register(&auth_cb_display);
    bt_conn_auth_info_cb_register(&auth_cb_info);

    err = bt_enable(NULL);
    if (err) {
        printk("[Keyfob] Bluetooth init failed (err %d)\n", err);
        return err;
    }

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        err = settings_load();
        if (err) {
            printk("[Keyfob] Error loading settings (err %d)\n", err);
        }
    }

    k_msleep(10);
    start_adv();

    return 0;
}

bool keyfob_is_in_range(void)
{
    return connected_and_secured;
}

void keyfob_reset_pairing_memory(void)
{
    int err;

    if (current_conn) {
        printk("[Keyfob] Force-disconnecting active Central...\n");
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        k_msleep(500); // Give stack time to gracefully terminate
    }

    err = bt_unpair(BT_ID_DEFAULT, NULL);
    if (err) {
        printk("[Keyfob] Error clearing flash bounds (err %d)\n", err);
    } else {
        printk("[Keyfob] SUCCESS: All bonding information cleared from Flash storage.\n");
    }
}