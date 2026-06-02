cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(i2c_lis3dh_wake)

target_sources(app PRIVATE 
    src/main.c
    src/imu_driver.c
)

target_include_directories(app PRIVATE 
    include
)