#include <zephyr/kernel.h>

void main(void)
{
    while (1) {
        printk("HELLO\n");
        k_sleep(K_SECONDS(1));
    }
}