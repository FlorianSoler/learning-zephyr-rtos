#include "certificates.h"

const unsigned char root_ca[] = {
#include "root.cert.pem.inc"
};
const size_t root_ca_len = sizeof(root_ca) - 1;

const unsigned char client_cert[] = {
#include "client.cert.pem.inc"
};
const size_t client_cert_len = sizeof(client_cert) - 1;

const unsigned char client_key[] = {
#include "client.key.pem.inc"
};
const size_t client_key_len = sizeof(client_key) - 1;