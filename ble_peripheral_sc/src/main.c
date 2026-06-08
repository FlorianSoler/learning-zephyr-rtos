/* main.c - Application main entry point */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/gpio.h>

/* =========================================================
 * DEVICETREE : Bouton de reset
 * ========================================================= */
#define RESET_BUTTON_NODE DT_ALIAS(reset_button)
#if !DT_NODE_HAS_STATUS_OKAY(RESET_BUTTON_NODE)
#define RESET_BUTTON_NODE DT_NODELABEL(reset_button)
#endif

static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

/* =========================================================
 * ÉTAT GLOBAL
 * ========================================================= */
static struct bt_conn *current_conn = NULL;

/* --- FIX PRINCIPAL : work item différé pour relancer l'adv ---
 * On ne doit JAMAIS appeler bt_le_adv_start() directement depuis
 * un callback BT (contexte système BLE). On délègue via un work
 * item pour laisser le contrôleur libérer ses buffers HCI/ACL.
 */
static struct k_work_delayable adv_work;

/* =========================================================
 * ADVERTISING DATA
 * ========================================================= */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* =========================================================
 * ADVERTISING : démarrage avec retry
 * ========================================================= */
static void start_adv(void)
{
    int err;

    /* Arrêt propre avant de relancer, au cas où un état résiduel traîne */
    (void)bt_le_adv_stop();
    k_msleep(50);

    for (int retry = 0; retry < 5; retry++) {
        err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                              ad, ARRAY_SIZE(ad),
                              sd, ARRAY_SIZE(sd));

        if (err == 0) {
            printk("Advertising started avec succes.\n");
            return;
        }

        printk("adv_start echec (err %d), tentative %d/5 - attente 200ms...\n",
               err, retry + 1);
        k_msleep(200);
    }

    /* Toutes les tentatives ont échoué : on reprogramme un essai
     * dans 2 secondes plutôt que de rester bloqué définitivement. */
    printk("[ERREUR] Advertising impossible apres 5 tentatives (err %d). "
           "Nouvel essai dans 2s...\n", err);
    k_work_schedule(&adv_work, K_MSEC(2000));
}

/* Handler du work item : appelé en dehors du contexte callback BLE */
static void adv_work_handler(struct k_work *work)
{
    start_adv();
}

/* =========================================================
 * CALLBACKS DE CONNEXION
 * ========================================================= */
static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Echec de connexion a %s : err %u (%s)\n",
               bt_conn_dst_str(conn), err, bt_hci_err_to_str(err));
        return;
    }

    printk("Connected : %s\n", bt_conn_dst_str(conn));

    /* Mise à jour de la référence de connexion active */
    if (current_conn) {
        bt_conn_unref(current_conn);
    }
    current_conn = bt_conn_ref(conn);

    err = bt_conn_set_security(conn, BT_SECURITY_L4);
    if (err) {
        printk("Echec set_security (err %d)\n", err);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected from %s, reason 0x%02x %s\n",
           bt_conn_dst_str(conn), reason, bt_hci_err_to_str(reason));

    if (current_conn == conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    /* FIX : on ne relance PAS l'adv directement ici.
     *
     * reason 0x08 = BT_HCI_ERR_CONN_TIMEOUT (coupure brutale) :
     *   le contrôleur a besoin de plus de temps pour libérer ses
     *   buffers ACL/HCI internes → délai de 1000 ms.
     *
     * Autres reasons (déconnexion propre) : 500 ms suffisent.
     */
    uint32_t delay_ms = (reason == BT_HCI_ERR_CONN_TIMEOUT) ? 1000U : 500U;

    printk("Relance de l'advertising dans %u ms...\n", delay_ms);
    k_work_schedule(&adv_work, K_MSEC(delay_ms));
}

static void identity_resolved(struct bt_conn *conn,
                               const bt_addr_le_t *rpa,
                               const bt_addr_le_t *identity)
{
    printk("Identity resolved : %s -> %s\n",
           bt_addr_le_str(rpa), bt_addr_le_str(identity));
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                              enum bt_security_err err)
{
    if (!err) {
        printk("Security changed : %s level %u\n",
               bt_conn_dst_str(conn), level);
    } else {
        printk("Security failed : %s level %u err %s(%d)\n",
               bt_conn_dst_str(conn), level,
               bt_security_err_to_str(err), err);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected        = connected,
    .disconnected     = disconnected,
    .identity_resolved = identity_resolved,
    .security_changed = security_changed,
};

/* =========================================================
 * CALLBACKS D'AUTHENTIFICATION
 * ========================================================= */
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    printk("Passkey pour %s : %06u\n", bt_conn_dst_str(conn), passkey);
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    printk("Code correspondant sur le smartphone ? : %06u\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_cancel(struct bt_conn *conn)
{
    printk("Appairage annule : %s\n", bt_conn_dst_str(conn));
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    printk("Appairage termine. Enregistre en flash : %s\n",
           bonded ? "OUI" : "NON");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    printk("Appairage echoue (%d). Deconnexion.\n", reason);
    bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .passkey_display = auth_passkey_display,
    .passkey_confirm = auth_passkey_confirm,
    .cancel          = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_cb_info = {
    .pairing_complete = pairing_complete,
    .pairing_failed   = pairing_failed,
};

/* =========================================================
 * BOUTON DE RESET : callback d'interruption
 * ========================================================= */
void reset_button_pressed_handler(const struct device *dev,
                                   struct gpio_callback *cb,
                                   uint32_t pins)
{
    static uint32_t last_time = 0;
    uint32_t now = k_uptime_get_32();
    int err;

    /* Anti-rebond logiciel 200 ms */
    if (now - last_time < 200U) {
        return;
    }
    last_time = now;

    printk("\n[Interrupteur] Basculement detecte : Reinitialisation BLE...\n");

    /* Déconnexion forcée si un appareil est connecté */
    if (current_conn) {
        printk("Deconnexion du smartphone actif...\n");
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }

    /* Suppression de tous les appairages en flash */
    err = bt_unpair(BT_ID_DEFAULT, NULL);
    if (err) {
        printk("Erreur suppression appairages (err %d)\n", err);
    } else {
        printk("SUCCES : Memoire flash videe. Anciens profils oublies.\n");
    }
}

/* =========================================================
 * INIT GPIO : bouton de reset
 * ========================================================= */
static int init_reset_button(void)
{
    int ret;

    if (!gpio_is_ready_dt(&reset_button)) {
        printk("Erreur : peripherique GPIO non pret.\n");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&reset_button, GPIO_INPUT);
    if (ret != 0) {
        printk("Erreur configuration broche %d (err %d)\n",
               reset_button.pin, ret);
        return ret;
    }

    /* Interruption sur front actif (descente si ACTIVE_LOW) */
    ret = gpio_pin_interrupt_configure_dt(&reset_button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) {
        printk("Erreur configuration interruption front (err %d)\n", ret);
        return ret;
    }

    gpio_init_callback(&button_cb_data,
                       reset_button_pressed_handler,
                       BIT(reset_button.pin));

    ret = gpio_add_callback(reset_button.port, &button_cb_data);
    if (ret != 0) {
        printk("Erreur gpio_add_callback (err %d)\n", ret);
        return ret;
    }

    printk("Bouton reset configure (Pull-up, Interrupt Edge Active).\n");
    return 0;
}

/* =========================================================
 * MAIN
 * ========================================================= */
int main(void)
{
    int err;

    printk("Initialisation Bluetooth + GPIO...\n");

    /* 1. Initialiser le work item différé AVANT tout le reste */
    k_work_init_delayable(&adv_work, adv_work_handler);

    /* 2. Initialiser le bouton de reset */
    err = init_reset_button();
    if (err) {
        printk("Echec init bouton reset (err %d)\n", err);
        return err;
    }

    /* 3. Enregistrer les callbacks de sécurité */
    bt_conn_auth_cb_register(&auth_cb_display);
    bt_conn_auth_info_cb_register(&auth_cb_info);

    /* 4. Activer le hardware BLE */
    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }
    printk("Bluetooth initialise.\n");

    /* 5. Charger les settings (bonds persistants en flash) */
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        err = settings_load();
        if (err) {
            printk("Erreur settings_load (err %d)\n", err);
        } else {
            printk("Settings charges avec succes.\n");
        }
    }

    k_msleep(10);

    /* 6. Démarrer l'advertising */
    start_adv();

    /* 7. Boucle principale : lecture polling du bouton de reset
     *    (complément à l'interruption, couvre les cas de rebond
     *     ou de signal maintenu bas) */
    printk("Surveillance bouton reset active (polling 100ms)...\n");

    while (1) {
        /* gpio_pin_get_dt() tient compte du flag GPIO_ACTIVE_LOW :
         * retourne 1 si la broche est à l'état ACTIF (= masse si ACTIVE_LOW) */
        int val = gpio_pin_get_dt(&reset_button);

        if (val == 1) {
            printk("\n[Polling] Niveau actif detecte sur bouton reset !\n");

            /* Déconnexion si nécessaire */
            if (current_conn) {
                printk("Deconnexion du smartphone actif...\n");
                bt_conn_disconnect(current_conn,
                                   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                k_msleep(500);
            }

            /* Effacement des clés en flash */
            err = bt_unpair(BT_ID_DEFAULT, NULL);
            if (err) {
                printk("Erreur suppression appairages (err %d)\n", err);
            } else {
                printk("SUCCES : Toutes les cles d'appairage supprimees.\n");
                printk("Remets l'interrupteur en position haute.\n");
            }

            /* Attente que le bouton soit relâché avant de continuer */
            while (gpio_pin_get_dt(&reset_button) == 1) {
                k_msleep(100);
            }
            printk("[Polling] Bouton relache. Systeme pret.\n");
            k_msleep(100);
            sys_reboot(SYS_REBOOT_COLD);
        }

        k_msleep(100);
    }

    return 0;
}