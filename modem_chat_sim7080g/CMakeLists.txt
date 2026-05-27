cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modem_chat_sim7080g)

# 1. Dynamically search the folder for all matching files
file(GLOB CLIENT_CERT_PATH "certificate/*.cert.pem")
file(GLOB CLIENT_KEY_PATH  "certificate/*.key.pem")

# 2. CRITICAL FIX: Filter out "root.cert.pem" so it isn't mistaken for a client cert
list(FILTER CLIENT_CERT_PATH EXCLUDE REGEX "root\\.cert\\.pem")

# 3. Safety Check: Ensure the automation pipeline dropped exactly 1 client certificate
list(LENGTH CLIENT_CERT_PATH cert_count)
if(NOT cert_count EQUAL 1)
    message(FATAL_ERROR "Automation Error: Expected exactly 1 client cert in folder, found: ${cert_count}")
endif()

# 4. Extract the dynamic UUID/Identity from the remaining file
get_filename_component(TMP_NAME ${CLIENT_CERT_PATH} NAME_WE)
get_filename_component(EXTRACTED_UUID ${TMP_NAME} NAME_WE)

# Set up include directory for the generated asset files
set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/certs)
zephyr_include_directories(${gen_dir})

# 5. Feed the files into the generator (root remains explicit, client uses the dynamic path)
generate_inc_file_for_target(app certificate/root.cert.pem  ${gen_dir}/root.cert.pem.inc)
generate_inc_file_for_target(app ${CLIENT_CERT_PATH}        ${gen_dir}/client.cert.pem.inc)
generate_inc_file_for_target(app ${CLIENT_KEY_PATH}         ${gen_dir}/client.key.pem.inc)

target_sources(app PRIVATE 
    src/main.c 
    src/modem_driver.c
    src/certificates.c
)

target_include_directories(app PRIVATE 
    include
)

# 6. Inject the dynamic string macro straight into your C compilation target
target_compile_definitions(app PRIVATE MQTT_CLIENT_ID="${EXTRACTED_UUID}")

# Status message for verification
message(STATUS ">>> AUTOMATION: Discovered Device Identity: ${EXTRACTED_UUID} <<<")