#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

int main(void)
{
    const struct net_if *iface;
    struct net_linkaddr *ll;

    LOG_INF("PPP demo start");

    iface = net_if_get_default();
    if (!iface) {
        LOG_ERR("No default interface");
        return 0;
    }

    ll = net_if_get_link_addr((struct net_if *)iface);
    LOG_INF("Interface ready: %s", ll ? ll->addr : "unknown");

    while (1) {
        k_sleep(K_SECONDS(5));
        LOG_INF("PPP app running");
    }
}