# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Locates the Zephyr CMake package registered by `west zephyr-export`
# (or uses ZEPHYR_BASE, which west sets automatically during `west build`).
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rtos_blinky)

target_sources(app PRIVATE src/main.c)
