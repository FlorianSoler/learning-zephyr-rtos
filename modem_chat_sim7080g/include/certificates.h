#ifndef CERTIFICATES_H
#define CERTIFICATES_H

#include <stddef.h>

extern const unsigned char root_ca[];
extern const size_t root_ca_len;
extern const unsigned char client_cert[];
extern const size_t client_cert_len;
extern const unsigned char client_key[];
extern const size_t client_key_len;

#endif