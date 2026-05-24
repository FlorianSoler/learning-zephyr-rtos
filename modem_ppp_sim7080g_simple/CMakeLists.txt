cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(modem_ppp_sim7080g_simple)

target_sources(app PRIVATE src/main.c)