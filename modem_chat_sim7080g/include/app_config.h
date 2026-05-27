#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* MQTT Broker Network Settings */
#define MQTT_BROKER_URL       "solanix.fr"
#define MQTT_BROKER_PORT      8883
#define MQTT_KEEP_ALIVE_SEC   60
#define MQTT_CLEAN_SESSION    1

/* Security Routing */
#define MQTT_SSL_CTX_INDEX    0  // Ties directly to the secure profile we provisioned

// NOTE: MQTT_CLIENT_ID is omitted here because it is automatically 
// injected at compile-time by CMake based on your certificate filename!

#endif // APP_CONFIG_H