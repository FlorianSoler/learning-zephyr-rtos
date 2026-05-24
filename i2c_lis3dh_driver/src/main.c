#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

/* Récupération automatique de l'instance définie dans le Devicetree */
#define LIS3DH_NODE DT_NODELABEL(lis3dh)

/* Fonction de rappel (Callback) appelée lors d'un mouvement */
static void trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
    if (trig->type == SENSOR_TRIG_DATA_READY || trig->type == SENSOR_TRIG_DELTA) {
        printk("Mouvement détecté par le driver LIS3DH !\n");
        
        /* 
         * Optionnel : Vous pouvez lire les données ici si nécessaire 
         * struct sensor_value accel[3];
         * sensor_sample_fetch(dev);
         * sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, accel);
         */
    }
}

int main(void)
{
    /* 1. Récupérer l'appareil via le Devicetree */
    const struct device *const imu_dev = DEVICE_DT_GET(LIS3DH_NODE);

    if (!device_is_ready(imu_dev)) {
        printk("Le capteur LIS3DH n'est pas prêt.\n");
        return -1;
    }
    printk("LIS3DH initialisé avec succès via le driver.\n");

    /* 2. Configurer le Trigger (Interruption) */
    struct sensor_trigger trig = {
        .type = SENSOR_TRIG_DELTA, // Déclenchement sur changement (mouvement)
        .chan = SENSOR_CHAN_ACCEL_XYZ,
    };

    /* 3. Enregistrer la fonction de callback */
    int rc = sensor_trigger_set(imu_dev, &trig, trigger_handler);
    if (rc < 0) {
        printk("Impossible d'activer le trigger : erreur %d\n", rc);
        return rc;
    }

    printk("Système actif. En attente d'interruptions sur la pin du Devicetree...\n");

    /* La boucle principale n'a plus besoin de "polder" la pin */
    while (1) {
        k_sleep(K_MSEC(1000));
    }

    return 0;
}