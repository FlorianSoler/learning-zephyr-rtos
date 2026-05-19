#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/poweroff.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

#define LED0_NODE DT_ALIAS(led0)
#define SW_PIN    31
#define BLINK_MS  200

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static struct gpio_callback sw_cb;
static struct k_work_delayable sleep_work;
static const struct device *gpio0_dev;

static void led_on(void)  { gpio_pin_set_dt(&led, 1); }
static void led_off(void) { gpio_pin_set_dt(&led, 0); }

static void go_to_sleep(struct k_work *work)
{
    led_off();
    nrf_gpio_cfg_sense_input(SW_PIN, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
    sys_poweroff();
}

static void sw_isr(const struct device *dev,
                   struct gpio_callback *cb,
                   uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    led_on();
    k_work_reschedule(&sleep_work, K_SECONDS(10));
}

static void blink_n(int n)
{
    for (int i = 0; i < n; i++) {
        led_on();
        k_msleep(BLINK_MS);
        led_off();
        k_msleep(BLINK_MS);
    }
}

static bool is_first_boot(void)
{
    uint32_t reas = nrf_power_resetreas_get(NRF_POWER);
    nrf_power_resetreas_clear(NRF_POWER, reas);
    return (reas == 0);
}

int main(void)
{
    gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio0_dev) || !device_is_ready(led.port)) {
        return 0;
    }

    /* LED off immediately — GPIO_OUTPUT_INACTIVE respects active-low in DTS */
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    /* P0.31 wake-up button */
    gpio_pin_configure(gpio0_dev, SW_PIN, GPIO_INPUT | GPIO_PULL_UP);
    gpio_pin_interrupt_configure(gpio0_dev, SW_PIN, GPIO_INT_EDGE_TO_ACTIVE);
    gpio_init_callback(&sw_cb, sw_isr, BIT(SW_PIN));
    gpio_add_callback(gpio0_dev, &sw_cb);

    k_work_init_delayable(&sleep_work, go_to_sleep);

    if (is_first_boot()) {
        blink_n(5);                              /* first power-on: blink 5x then sleep */
    } else {
        led_on();                                /* wake from System OFF: LED on, start timer */
        k_work_reschedule(&sleep_work, K_SECONDS(10));
        /* let the kernel run the timer — fall through to poweroff setup */
        /* sleep_work will call go_to_sleep() after 10s                  */
        nrf_gpio_cfg_sense_input(SW_PIN, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
        return 0;                                /* return to kernel, work queue runs */
    }

    /* first boot path — go to sleep after blinks */
    nrf_gpio_cfg_sense_input(SW_PIN, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);
    sys_poweroff();

    return 0;
}