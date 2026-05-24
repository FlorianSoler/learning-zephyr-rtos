#ifndef MODEM_SERVICE_H_
#define MODEM_SERVICE_H_

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/backend/uart.h>

typedef struct {
    const char *url;
    uint16_t port;
    const char *client_id;
    uint16_t keep_time;
    uint8_t clean_session;
    uint8_t qos;
    uint8_t retain;
    const char *base_topic;
} mqtt_config_t;

struct modem_service {
    const struct device *uart_dev;
    struct gpio_dt_spec power_key_gpio;
    struct gpio_dt_spec power_nfet_gpio;
    
    struct modem_pipe *uart_pipe;
    struct modem_backend_uart uart_backend;
    struct modem_chat chat_ctx;
    
    uint8_t chat_receive_buf[512];
    uint8_t uart_backend_buf[1024];
    
    mqtt_config_t mqtt_config;
};

int modem_service_init(struct modem_service *ctx, const struct device *uart_dev,
                       const struct gpio_dt_spec *power_key, 
                       const struct gpio_dt_spec *power_nfet,
                       mqtt_config_t mqtt_config);

void modem_power_on(struct modem_service *ctx);
void modem_power_off(struct modem_service *ctx);
void modem_enable_lte(struct modem_service *ctx);
void modem_disable_lte(struct modem_service *ctx);
void modem_setup_mqtt(struct modem_service *ctx);
void modem_send_mqtt_message(struct modem_service *ctx, const char *sub_topic, const char *message);
void modem_disconnect_mqtt(struct modem_service *ctx);

#endif /* MODEM_SERVICE_H_ */