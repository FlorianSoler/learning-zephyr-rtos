cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modem_chat_sim7080g)

set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/certs)
zephyr_include_directories(${gen_dir})

generate_inc_file_for_target(app certificate/root.cert.pem  ${gen_dir}/root.cert.pem.inc)
generate_inc_file_for_target(app certificate/client_4.cert.pem   ${gen_dir}/client.cert.pem.inc)
generate_inc_file_for_target(app certificate/client_4.key.pem   ${gen_dir}/client.key.pem.inc)

target_sources(app PRIVATE 
    src/main.c 
    src/modem_driver.c
    src/certificates.c
)

target_include_directories(app PRIVATE 
    include
)