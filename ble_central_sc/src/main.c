/* main.c - Application Centrale Bluetooth avec gestion de la LED0, Persistance, Timeout et Reset Manuel */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/gpio.h>

// --- CONFIGURATION DU NOM CIBLE ---
#define TARGET_NAME "SC only peripheral"

// --- CONFIGURATION DU MATÉRIEL DEVICETREE ---
#define PAIR_BUTTON_NODE  DT_PATH(buttons, button_0)
#define RESET_BUTTON_NODE DT_PATH(buttons, button_1) // <-- AJOUTÉ : bouton de reset matériel
#define LED0_NODE         DT_ALIAS(led0)

static const struct gpio_dt_spec pair_button  = GPIO_DT_SPEC_GET(PAIR_BUTTON_NODE, gpios);
static const struct gpio_dt_spec reset_button = GPIO_DT_SPEC_GET(RESET_BUTTON_NODE, gpios); // <-- AJOUTÉ
static const struct gpio_dt_spec led0         = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static struct bt_conn *default_conn = NULL;
static bool bond_found = false;
static bool allow_pairing = false;

// Tâches différées (Work Queue)
static struct k_work_delayable security_work;
static struct k_work_delayable connection_timeout_work;

// Callback d'analyse des paquets publicitaires reçus
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

// Vérification si l'adresse est déjà enregistrée en mémoire Flash
static void check_bond_cb(const struct bt_bond_info *info, void *user_data)
{
    bool *found = user_data;
    *found = true;
    
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(&info->addr, addr_str, sizeof(addr_str));
    printk("[Flash] Liaison existante trouvee avec : %s\n", addr_str);
}

// Callback de gestion du Timeout de connexion
static void connection_timeout_handler(struct k_work *work)
{
    if (default_conn) {
        printk("[Centrale] TIMEOUT : Le périphérique n'a pas répondu. Annulation de la tentative...\n");
        
        bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(default_conn);
        default_conn = NULL;

        allow_pairing = false; 

        printk("[Centrale] Relance automatique du scan...\n");
        bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
    }
}

// Callback appelée à chaque détection d'un signal Bluetooth publicitaire
static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
    char name[32] = {0};
    int err;

    bt_data_parse(ad, parse_device_name, name);

    if (strcmp(name, TARGET_NAME) == 0) {
        if (bond_found || allow_pairing) {
            printk("[Centrale] CIBLE TROUVEE : '%s'. Arret du scan et connexion...\n", name);
            
            err = bt_le_scan_stop();
            if (err) {
                printk("Impossible d'arreter le scan (err %d)\n", err);
                return;
            }

            err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN, 
                                    BT_LE_CONN_PARAM_DEFAULT, &default_conn);
            if (err) {
                printk("Echec du lancement de la connexion (err %d). Reprise du scan...\n", err);
                bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
            } else {
                // Sécurité : Si la connexion met plus de 5 secondes, le timeout se déclenche
                k_work_schedule(&connection_timeout_work, K_MSEC(5000));
            }
        }
    }
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = scan_recv,
};

// Fonction exécutée en différé pour demander la sécurité sans entrer en conflit
static void security_initiate_work(struct k_work *work)
{
    if (!default_conn) {
        return;
    }

    printk("[Centrale] Envoi proactif de la demande de securite L4...\n");
    int err = bt_conn_set_security(default_conn, BT_SECURITY_L4);
    
    if (err && err != -EALREADY) {
        printk("Erreur critique lors de la demande de securite L4 (err %d)\n", err);
    }
}

// --- CALLBACKS DE CONNEXION ---
static void connected(struct bt_conn *conn, uint8_t err)
{
    k_work_cancel_delayable(&connection_timeout_work);

    if (err) {
        printk("[Centrale] Echec de la connexion (err %u)\n", err);
        if (default_conn == conn) {
            bt_conn_unref(default_conn);
            default_conn = NULL;
        }
        allow_pairing = false;
        bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
        return;
    }

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));
    printk("[Centrale] Connecte a : %s\n", addr_str);

    // --- ALLUMER LA LED0 ---
    gpio_pin_set_dt(&led0, 1);

    k_work_schedule(&security_work, K_MSEC(50));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("[Centrale] Deconnecte. Raison: 0x%02x\n", reason);

    // --- ÉTEINDRE LA LED0 ---
    gpio_pin_set_dt(&led0, 0);

    k_work_cancel_delayable(&security_work);
    k_work_cancel_delayable(&connection_timeout_work);

    if (default_conn == conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }

    // Mise à jour de l'état des clés locales stockées en Flash
    bond_found = false;
    bt_foreach_bond(BT_ID_DEFAULT, check_bond_cb, &bond_found);
    allow_pairing = false; 

    printk("[Centrale] Relance du scan...\n");
    bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    if (!err) {
        printk("[Centrale] SECURITE ETABLIE ! Niveau actuel : %u\n", level);
        bond_found = true; 
    } else {
        // --- MODIFIÉ : Pas d'effacement automatique. On coupe juste le lien erroné.
        // L'utilisateur devra appuyer sur le bouton_1 s'il souhaite forcer un reset de la clé locale.
        printk("[Centrale] Echec de la securisation (Erreur pile: %d). Coupure du lien errone.\n", err);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

// --- CALLBACKS DE SECURITE ---
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    printk("\n==================================\n");
    printk("  CODE D'APPAIRAGE CENTRALE : %06u\n", passkey);
    printk("==================================\n");
}

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    printk("[Centrale] Validation automatique du code SC : %06u\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_cancel(struct bt_conn *conn)
{
    printk("[Centrale] Appairage annule.\n");
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    printk("Appairage reussi ! Sauvegarde en Flash : %s\n", bonded ? "OUI" : "NON");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    printk("Echec de l'appairage (Raison: %d). Coupure.\n", reason);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .passkey_display = auth_passkey_display,
    .passkey_confirm = auth_passkey_confirm,
    .cancel          = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_cb_info = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

int init_hardware(void)
{
    int ret;

    // Bouton_0 (Appairage)
    if (!gpio_is_ready_dt(&pair_button)) {
        printk("Erreur : Le GPIO du bouton d'appairage n'est pas pret.\n");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&pair_button, GPIO_INPUT);
    if (ret != 0) return ret;

    // Bouton_1 (Reset Clés / Flash)
    if (!gpio_is_ready_dt(&reset_button)) {
        printk("Erreur : Le GPIO du bouton de reset n'est pas pret.\n");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&reset_button, GPIO_INPUT);
    if (ret != 0) return ret;

    // LED0
    if (!gpio_is_ready_dt(&led0)) {
        printk("Erreur : Le GPIO de la LED0 n'est pas pret.\n");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) return ret;

    return 0;
}

int main(void)
{
    int err;

    printk("Demarrage de la Centrale Bluetooth Sûre...\n");

    err = init_hardware();
    if (err) return err;

    k_work_init_delayable(&security_work, security_initiate_work);
    k_work_init_delayable(&connection_timeout_work, connection_timeout_handler);

    err = bt_enable(NULL);
    if (err) {
        printk("Echec de l'activation du Bluetooth (err %d)\n", err);
        return err;
    }

    printk("Puce Bluetooth activee.\n");

    bt_conn_auth_cb_register(&auth_cb_display);
    bt_conn_auth_info_cb_register(&auth_cb_info);

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        err = settings_load();
        if (err) {
            printk("Erreur lors du chargement des cles (err %d)\n", err);
        } else {
            printk("Cles d'appairage Flash chargees avec succes.\n");
        }
    }

    // Vérification initiale des liaisons en Flash
    bond_found = false;
    bt_foreach_bond(BT_ID_DEFAULT, check_bond_cb, &bond_found);

    if (bond_found) {
        printk("[Boot] Liaison trouvee en Flash. Reconnexion automatique active.\n");
    } else {
        printk("[Boot] Aucune liaison en Flash. Appuyez sur bouton_0 pour appairer.\n");
    }

    bt_le_scan_cb_register(&scan_callbacks);

    struct bt_le_scan_param scan_param = {
        .type       = BT_LE_SCAN_TYPE_ACTIVE, 
        .options    = BT_LE_SCAN_OPT_NONE,    
        .interval   = BT_GAP_SCAN_FAST_INTERVAL,
        .window     = BT_GAP_SCAN_FAST_WINDOW,
    };

    err = bt_le_scan_start(&scan_param, NULL);
    if (err) {
        printk("Echec du lancement du scan (err %d)\n", err);
        return err;
    }

    printk("Scan actif global lance. En attente de la cible...\n");

    while (1) {
        // --- 1. GESTION DU BOUTON_0 : LANCEMENT APPAIRAGE ---
        if (!bond_found) {
            if (gpio_pin_get_dt(&pair_button) == 1) {
                if (!allow_pairing) {
                    allow_pairing = true;
                    printk("\n[Bouton_0] Mode appairage active ! Autorisation de connexion a '%s'...\n", TARGET_NAME);
                }
            }
        }

        // --- 2. GESTION DU BOUTON_1 : RESET MANUEL DES CLÉS DE LA FLASH ---
        if (gpio_pin_get_dt(&reset_button) == 1) {
            printk("\n[Bouton_1] Reset manuel detecte ! Nettoyage des clés Bluetooth...\n");

            // Déconnexion forcée du lien en cours si existant
            if (default_conn) {
                printk("[Reset] Fermeture de la connexion active...\n");
                bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
                k_msleep(400); // Laisse le temps à la pile d'exécuter la déconnexion
            }

            // Suppression définitive de toutes les clés d'appairage stockées localement
            int unpair_err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
            if (unpair_err) {
                printk("[Reset] Erreur lors de la suppression des liaisons (err %d)\n", unpair_err);
            } else {
                printk("[Reset] SUCCÈS : Mémoire Flash réinitialisée. Toutes les clés sont effacées !\n");
            }

            // Réinitialisation des états locaux
            bond_found = false;
            allow_pairing = false;

            // Anti-redondance : On bloque tant que le bouton_1 reste pressé
            while (gpio_pin_get_dt(&reset_button) == 1) {
                k_msleep(100);
            }
            printk("[Reset] Bouton_1 relâché. Centrale prête pour un tout nouvel appairage.\n");
        }

        k_msleep(100);
    }
    return 0;
}