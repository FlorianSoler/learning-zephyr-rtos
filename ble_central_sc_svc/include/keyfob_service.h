#ifndef KEYFOB_SERVICE_H_
#define KEYFOB_SERVICE_H_

#include <stdbool.h>

int keyfob_init(void);
bool keyfob_is_in_range(void);
void keyfob_allow_pairing(void);

#endif /* KEYFOB_SERVICE_H_ */