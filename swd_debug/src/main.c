#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{
    while (1) {
        LOG_INF("Hello via RTT!");
        k_sleep(K_MSEC(1000));
    }
}