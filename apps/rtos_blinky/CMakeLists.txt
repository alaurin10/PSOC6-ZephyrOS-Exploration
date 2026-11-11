# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Locates the Zephyr CMake package registered by `west zephyr-export`
# (or uses ZEPHYR_BASE, which west sets automatically during `west build`).
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rtos_blinky)

target_sources(app PRIVATE
  src/main.c
  src/thermistor.c
  src/app_settings.c
  src/app_watchdog.c
  src/channels.c
  src/led_ctrl.c
  src/temp_monitor.c
)
