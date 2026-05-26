maybe firmware bug but you have to upload all to index 3 :

    if (modem_write_file(uart1_dev, >>3<<here, "root_ca.pem", (const uint8_t *)root_ca, sizeof(root_ca)- 1, 2) != 0) {

    if (modem_write_file(uart1_dev, 3, "client_cert.pem", (const uint8_t *)client_cert, sizeof(client_cert)- 1, 1) != 0) {

    if (modem_write_file(uart1_dev, 3, "client_key.pem", (const uint8_t *)client_key, sizeof(client_key)- 1, 3) != 0) {


for the key use traditional rsa format header


AT+CSSLCFG="CONVERT",2,"root_ca.pem"
AT+CSSLCFG="CONVERT",1,"client_cert.pem","client_key.pem"


ecc deubg :

# 1. Create a Test Root CA
openssl req -x509 -newkey rsa:2048 -days 365 -nodes -keyout test_ca.key -out test_ca.pem -subj "/CN=Test_Root_CA"

# 2. Create the Traditional RSA Private Key (The command you verified!)
openssl genrsa -out traditional_rsa_key.pem -traditional 2048

# 3. Create CSR and sign it to make the Client Certificate
openssl req -new -key traditional_rsa_key.pem -out rsa_client.csr -subj "/CN=RSA_Client"
openssl x509 -req -in rsa_client.csr -CA test_ca.pem -CAkey test_ca.key -CAcreateserial -out rsa_client_cert.pem -days 365

working :

# 1. Generate raw ECC private parameters
openssl ecparam -name prime256v1 -genkey -noout -out ecc_raw.pem

# 2. Force traditional SEC1 format (-----BEGIN EC PRIVATE KEY-----)
openssl ec -in ecc_raw.pem -out traditional_ecc_key.pem

# 3. Create CSR and sign it to create the ECC Client Certificate
openssl req -new -key traditional_ecc_key.pem -out ecc_client.csr -subj "/CN=ECC_Client"
openssl x509 -req -in ecc_client.csr -CA test_ca.pem -CAkey test_ca.key -CAcreateserial -out ecc_client_cert.pem -days 365


mqtt + tls 

AT+SMDISC

AT+SMCONF="URL",solanix.fr,8883
AT+SMCONF="CLIENTID","client_4"
AT+SMCONF="KEEPTIME",60
AT+SMCONF="CLEANSS",1
AT+CSSLCFG="SSLVERSION",0,3
AT+CSSLCFG="CONVERT",2,"root_ca.pem"
AT+CSSLCFG="CONVERT",1,"client_cert.pem","client_key.pem"
AT+SMSSL=1,"root_ca.pem","client_cert.pem"
AT+SMCONN