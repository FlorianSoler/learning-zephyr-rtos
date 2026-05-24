#include "modem_service.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(modem_sim7080, LOG_LEVEL_INF);

static int run_at_cmd(struct modem_service *ctx, const char *cmd, const char *match, uint32_t timeout_ms)
{
    struct modem_chat_match response_match = {
        .match = match,
        .match_size = strlen(match),
    };

    struct modem_chat_match abort_matches[] = {
        { .match = "ERROR", .match_size = 5 },
        { .match = "CME ERROR", .match_size = 9 },
        { .match = "CMS ERROR", .match_size = 9 }
    };

    struct modem_chat_script_chat chat_line = {
        .request = cmd,
        .request_size = strlen(cmd),
        .response_matches = &response_match,
        .response_matches_size = 1,
        /* FIX : Attribue directement l'entier en millisecondes requis par la structure */
        .timeout = (uint16_t)timeout_ms, 
    };

    struct modem_chat_script script = {
        .name = "at_cmd",
        .script_chats = &chat_line,
        .script_chats_size = 1, 
        .abort_matches = abort_matches,
        .abort_matches_size = 3,
    };

    return modem_chat_run_script(&ctx->chat_ctx, &script);
}

int modem_service_init(struct modem_service *ctx, const struct device *uart_dev,
                       const struct gpio_dt_spec *power_key, 
                       const struct gpio_dt_spec *power_nfet,
                       mqtt_config_t mqtt_config)
{
    int ret;
    ctx->uart_dev = uart_dev;
    ctx->power_key_gpio = *power_key;
    ctx->power_nfet_gpio = *power_nfet;
    ctx->mqtt_config = mqtt_config;

    if (!device_is_ready(ctx->power_key_gpio.port) || !device_is_ready(ctx->power_nfet_gpio.port)) {
        LOG_ERR("GPIOs matériels non prêts");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&ctx->power_key_gpio, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&ctx->power_nfet_gpio, GPIO_OUTPUT_INACTIVE);

    const struct modem_backend_uart_config uart_backend_cfg = {
        .uart = ctx->uart_dev,
        .receive_buf = ctx->uart_backend_buf,
        .receive_buf_size = sizeof(ctx->uart_backend_buf),
    };
    
    ctx->uart_pipe = modem_backend_uart_init(&ctx->uart_backend, &uart_backend_cfg);
    if (!ctx->uart_pipe) {
        LOG_ERR("Impossible de créer le backend UART");
        return -EINVAL;
    }

    struct modem_chat_config chat_cfg = {
        .user_data = ctx,
        .receive_buf = ctx->chat_receive_buf,
        .receive_buf_size = sizeof(ctx->chat_receive_buf),
        .delimiter = "\r\n",
        .delimiter_size = 2,
    };
    
    ret = modem_chat_init(&ctx->chat_ctx, &chat_cfg);
    if (ret < 0) {
        LOG_ERR("Échec d'initialisation de modem_chat (%d)", ret);
        return ret;
    }

    ret = modem_chat_attach(&ctx->chat_ctx, ctx->uart_pipe);
    if (ret < 0) {
        LOG_ERR("Impossible d'attacher le pipe UART au chat (%d)", ret);
        return ret;
    }

    ret = modem_pipe_open(ctx->uart_pipe, K_MSEC(100));
    if (ret < 0) {
        LOG_ERR("Impossible d'ouvrir le canal UART physique (%d)", ret);
        return ret;
    }

    LOG_INF("Service Modem initialisé et prêt à l'emploi.");
    return 0;
}

void modem_power_on(struct modem_service *ctx)
{
    LOG_INF("Allumage du matériel (NFET à 1)...");
    gpio_pin_set_dt(&ctx->power_nfet_gpio, 1); 
    k_sleep(K_MSEC(500)); 

    if (run_at_cmd(ctx, "AT\r\n", "OK", 1000) == 0) {
        LOG_INF("Module SIM7080 déjà en ligne.");
        return;
    }

    LOG_INF("Pas de réponse. Envoi de l'impulsion PWRKEY...");
    gpio_pin_set_dt(&ctx->power_key_gpio, 1); 
    k_sleep(K_MSEC(1500)); 
    gpio_pin_set_dt(&ctx->power_key_gpio, 0);
    
    LOG_INF("Attente du boot du firmware SIM7080...");
    k_sleep(K_SECONDS(4)); 

    for (int i = 0; i < 5; i++) {
        if (run_at_cmd(ctx, "AT\r\n", "OK", 1000) == 0) {
            LOG_INF("SIM7080 a répondu OK !");
            return;
        }
        k_sleep(K_MSEC(500));
    }
    LOG_ERR("Le SIM7080 ne répond pas.");
}

void modem_power_off(struct modem_service *ctx)
{
    if (run_at_cmd(ctx, "AT\r\n", "OK", 500) == 0) {
        run_at_cmd(ctx, "AT+CPOWD=1\r\n", "NORMAL POWER DOWN", 2000);
        k_sleep(K_SECONDS(2));
    }
    gpio_pin_set_dt(&ctx->power_key_gpio, 0);
    gpio_pin_set_dt(&ctx->power_nfet_gpio, 0);
    LOG_INF("Modem éteint.");
}

void modem_enable_lte(struct modem_service *ctx)
{
    LOG_INF("Activation du réseau LTE...");
    run_at_cmd(ctx, "AT+CFUN=1\r\n", "OK", 5000);
    k_sleep(K_SECONDS(2));
    run_at_cmd(ctx, "AT+CGATT=1\r\n", "OK", 5000);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "AT+CGDCONT=1,\"IP\",\"TM\"\r\n"); 
    run_at_cmd(ctx, buf, "OK", 2000);
    
    run_at_cmd(ctx, "AT+CNACT=0,1\r\n", "OK", 5000);
}

void modem_disable_lte(struct modem_service *ctx)
{
    run_at_cmd(ctx, "AT+CNACT=0,0\r\n", "OK", 2000);
    run_at_cmd(ctx, "AT+CFUN=0\r\n", "OK", 5000);
}

void modem_setup_mqtt(struct modem_service *ctx)
{
    char cmdbuf[256];
    LOG_INF("Configuration de la session MQTT...");

    snprintf(cmdbuf, sizeof(cmdbuf), "AT+SMCONF=\"URL\",\"%s\",%d\r\n", ctx->mqtt_config.url, ctx->mqtt_config.port);
    run_at_cmd(ctx, cmdbuf, "OK", 2000);

    snprintf(cmdbuf, sizeof(cmdbuf), "AT+SMCONF=\"CLIENTID\",\"%s\"\r\n", ctx->mqtt_config.client_id);
    run_at_cmd(ctx, cmdbuf, "OK", 2000);

    snprintf(cmdbuf, sizeof(cmdbuf), "AT+SMCONF=\"KEEPTIME\",%d\r\n", ctx->mqtt_config.keep_time);
    run_at_cmd(ctx, cmdbuf, "OK", 2000);

    snprintf(cmdbuf, sizeof(cmdbuf), "AT+SMCONF=\"CLEANSS\",%d\r\n", ctx->mqtt_config.clean_session);
    run_at_cmd(ctx, cmdbuf, "OK", 2000);

    run_at_cmd(ctx, "AT+SMCONN\r\n", "OK", 20000);
}

void modem_send_mqtt_message(struct modem_service *ctx, const char *sub_topic, const char *message)
{
    char pub_cmd[256];
    snprintf(pub_cmd, sizeof(pub_cmd), "AT+SMPUB=\"%s%s\",%d,%d,%d\r\n", 
             ctx->mqtt_config.base_topic, sub_topic, (int)strlen(message), 
             ctx->mqtt_config.qos, ctx->mqtt_config.retain);

    if (run_at_cmd(ctx, pub_cmd, ">", 2000) == 0) {
        char msg_buf[256];
        snprintf(msg_buf, sizeof(msg_buf), "%s\r\n", message);
        run_at_cmd(ctx, msg_buf, "OK", 5000);
        LOG_INF("Message envoyé avec succès.");
    } else {
        LOG_ERR("Le modem n'a pas ouvert le prompt MQTT (pas de '>')");
    }
}

void modem_disconnect_mqtt(struct modem_service *ctx)
{
    run_at_cmd(ctx, "AT+SMDISC\r\n", "OK", 5000);
}