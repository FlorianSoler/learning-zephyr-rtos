#ifndef KEYFOB_SERVICE_H_
#define KEYFOB_SERVICE_H_

#include <stdbool.h>

/**
 * @brief Initializes the peripheral BLE hardware stack and starts advertising.
 * @return 0 on success, negative error code on failure.
 */
int keyfob_init(void);

/**
 * @brief Checks if the keyfob is currently in range (securely connected to the central).
 * @return true if connected and secured, false otherwise.
 */
bool keyfob_is_in_range(void);

/**
 * @brief Forces a disconnect of any current session and purges all saved bonds from flash.
 */
void keyfob_reset_pairing_memory(void);

#endif /* KEYFOB_SERVICE_H_ */