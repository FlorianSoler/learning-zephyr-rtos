#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#include <hal/nrf_gpio.h>

#define WAKEUP_PIN 11

static void configure_wakeup_pin(void)
{

    nrf_gpio_cfg_sense_input(
        WAKEUP_PIN,
        NRF_GPIO_PIN_PULLDOWN,
        NRF_GPIO_PIN_SENSE_HIGH
    );
}

int main(void)
{
    k_sleep(K_SECONDS(2));

    configure_wakeup_pin();

    k_sleep(K_MSEC(100));

    sys_poweroff();

    while (1) {
    }
}