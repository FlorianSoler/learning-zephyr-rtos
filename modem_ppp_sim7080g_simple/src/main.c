#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>

void main(void) {
    struct net_if *iface = net_if_get_default();
    
    printk("Attente de la connexion PPP...\n");
    
    while (!net_if_is_up(iface)) {
        k_sleep(K_MSEC(500));
    }

    printk("Connecté à Internet via PPP ! Le module SIM7080 est prêt.\n");
    // Vous pouvez maintenant utiliser les sockets standards (BSD) de Zephyr
}