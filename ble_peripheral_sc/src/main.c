/* main.c - Application main entry point */

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
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/gpio.h> // <-- AJOUTÉ pour la gestion des pins

// Récupération de la configuration du bouton depuis le devicetree
#define RESET_BUTTON_NODE DT_ALIAS(reset_button)
#if !DT_NODE_HAS_STATUS_OKAY(RESET_BUTTON_NODE)
#define RESET_BUTTON_NODE DT_NODELABEL(reset_button)
#endif

static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

// Variable globale ou pointeur pour suivre la connexion active
static struct bt_conn *current_conn = NULL;

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_adv(void)
{
    int err;

    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
    } else {
        printk("Advertising successfully started\n");
    }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Failed to connect to %s %u %s\n", bt_conn_dst_str(conn),
               err, bt_hci_err_to_str(err));
        return;
    }

    printk("Connected %s\n", bt_conn_dst_str(conn));
    
    // On garde une référence de la connexion pour pouvoir la couper
    if (current_conn) {
        bt_conn_unref(current_conn);
    }
    current_conn = bt_conn_ref(conn);

    err = bt_conn_set_security(conn, BT_SECURITY_L4);
    if (err) {
        printk("Failed to set security (err %d)\n", err);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected from %s, reason 0x%02x %s\n", bt_conn_dst_str(conn),
           reason, bt_hci_err_to_str(reason));
    
    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    start_adv();
}

static void identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa,
                  const bt_addr_le_t *identity)
{
    printk("Identity resolved %s -> %s\n", bt_addr_le_str(rpa), bt_addr_le_str(identity));
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                 enum bt_security_err err)
{
    if (!err) {
        printk("Security changed: %s level %u\n", bt_conn_dst_str(conn), level);
    } else {
        printk("Security failed: %s level %u err %s(%d)\n", bt_conn_dst_str(conn), level,
               bt_security_err_to_str(err), err);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .identity_resolved = identity_resolved,
    .security_changed = security_changed,
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    printk("Passkey for %s: %06u\n", bt_conn_dst_str(conn), passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    printk("Le code correspond-il sur le smartphone ? : %06u\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_cancel(struct bt_conn *conn)
{
    printk("Pairing cancelled: %s\n", bt_conn_dst_str(conn));
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    printk("Pairing Complete. Enregistre en flash : %s\n", bonded ? "OUI" : "NON");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    printk("Pairing Failed (%d). Disconnecting.\n", reason);
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

// --- FONCTION DE CALLBACK DU BOUTON (INTERRUPTION) ---
void reset_button_pressed_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    static uint32_t last_time = 0;
    uint32_t now = k_uptime_get_32();
    int err;

    // Anti-rebond logiciel de 200 ms pour l'interrupteur
    if (now - last_time < 200) {
        return;
    }
    last_time = now;

    printk("\n[Interrupteur] Basculement detecte : Reinitialisation de la memoire BLE...\n");

    // 1. Déconnexion forcée du téléphone actuel si connecté
    if (current_conn) {
        printk("Deconnexion du smartphone actif...\n");
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }

    // 2. Oubli définitif de tous les appairages
    err = bt_unpair(BT_ID_DEFAULT, NULL);
    if (err) {
        printk("Erreur lors de la suppression des appairages (err %d)\n", err);
    } else {
        printk("SUCCES : La memoire flash a ete videe. Anciens profils oublies.\n");
    }
}

// Configuration et initialisation du GPIO
int init_reset_button(void)
{
    int ret;

    if (!gpio_is_ready_dt(&reset_button)) {
        printk("Erreur : Le peripherique GPIO n'est pas pret.\n");
        return -ENODEV;
    }

    // Configure la broche en Entree + Pull-Up
    ret = gpio_pin_configure_dt(&reset_button, GPIO_INPUT);
    if (ret != 0) {
        printk("Erreur de configuration de la broche %d (err %d)\n", reset_button.pin, ret);
        return ret;
    }

    // Configure l'interruption sur niveau BAS (Sens Low)
    ret = gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_LEVEL_LOW);
    if (ret != 0) {
        printk("Erreur de configuration de l'interruption (err %d)\n", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) {
        printk("Erreur de configuration de l'interruption sur front (err %d)\n", ret);
        return ret;
    }
    printk("Broche P0.31 (Reset) configuree avec succes (Pull-up, Interrupt Low).\n");
    return 0;
}

int main(void)
{
    int err;

    printk("Initialisation du Bluetooth et des GPIO...\n");

    // 1. Initialiser le bouton de reset
    err = init_reset_button();
    if (err) {
        return err;
    }

    // 2. Enregistrer les structures de sécurité
    bt_conn_auth_cb_register(&auth_cb_display);
    bt_conn_auth_info_cb_register(&auth_cb_info);

    // 3. Activer le hardware BLE
    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }

    printk("Bluetooth initialized\n");

    // 4. Charger les settings
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        err = settings_load();
        if (err) {
            printk("Erreur settings_load (err %d)\n", err);
        } else {
            printk("Settings charges avec succes.\n");
        }
    }

    k_msleep(10);

    // 5. Lancer l'advertising
    start_adv();

    printk("Filtre manuel de l'interrupteur actif. En attente d'un niveau BAS...\n");

    while (1) {
        // gpio_pin_get_dt prend en compte le flag GPIO_ACTIVE_LOW.
        // Si la broche est reliée à la masse, val va valoir 1 (Actif).
        int val = gpio_pin_get_dt(&reset_button);

        if (val == 1) { 
            int err;
            printk("\n[Interrupteur] Niveau BAS detecte sur P0.31 ! Reinitialisation...\n");

            // 1. Déconnexion si un appareil est là
            if (current_conn) {
                printk("Deconnexion du smartphone actif...\n");
                bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                k_msleep(500); // On laisse le temps au contrôleur de couper
            }

            // 2. Effacement des clés en Flash
            err = bt_unpair(BT_ID_DEFAULT, NULL);
            if (err) {
                printk("Erreur lors de la suppression des appairages (err %d)\n", err);
            } else {
                printk("SUCCES : Toutes les cles d'appairage ont ete supprimees !\n");
                printk("S'il te plait, remets l'interrupteur en position haute.\n");
            }

            // 3. Anti-redondance : tant que l'interrupteur reste bas, on bloque ici
            // pour éviter d'effacer la flash en boucle toutes les millisecondes.
            while (gpio_pin_get_dt(&reset_button) == 1) {
                k_msleep(100); 
            }
            printk("[Interrupteur] Remis en position haute. Pret.\n");
        }

        k_msleep(100); // On vérifie l'état de l'interrupteur 10 fois par seconde
    }

    return 0;
}