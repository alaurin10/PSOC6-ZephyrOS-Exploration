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
  src/ble_beacon.c
  src/capsense_input.c
)

# ---- CapSense -----------------------------------------------------------------
# Infineon CapSense middleware (Infineon EULA, not committed): cloned once into
# the west workspace's modules/ folder, next to the other fetched code.
set(CAPSENSE_VERSION release-v3.0.1)
get_filename_component(WEST_WORKSPACE ${ZEPHYR_BASE} DIRECTORY)
set(CAPSENSE_DIR ${WEST_WORKSPACE}/modules/lib/capsense)

if(NOT EXISTS ${CAPSENSE_DIR}/cy_capsense.h)
  message(STATUS "Fetching Infineon CapSense middleware ${CAPSENSE_VERSION}")
  execute_process(
    COMMAND git clone --depth 1 --branch ${CAPSENSE_VERSION}
            https://github.com/Infineon/capsense ${CAPSENSE_DIR}
    RESULT_VARIABLE capsense_clone_result)
  if(NOT capsense_clone_result EQUAL 0)
    message(FATAL_ERROR "Could not clone the CapSense middleware into ${CAPSENSE_DIR}")
  endif()
endif()

file(GLOB CAPSENSE_SOURCES ${CAPSENSE_DIR}/*.c)
set(CAPSENSE_THIRD_PARTY_SOURCES
  ${CAPSENSE_SOURCES}
  capsense/cycfg_capsense.c
  # PDL driver for the CSD hardware block; the HAL module doesn't build it.
  ${ZEPHYR_HAL_INFINEON_MODULE_DIR}/mtb-pdl-cat1/drivers/source/cy_csd.c
)
target_sources(app PRIVATE ${CAPSENSE_THIRD_PARTY_SOURCES})
target_include_directories(app PRIVATE capsense ${CAPSENSE_DIR})

# Closed-source parts (SmartSense auto-tuning, advanced filters). The SOFTFP
# build is plain Armv6-M code with no FPU instructions, matching our build.
zephyr_link_libraries(${CAPSENSE_DIR}/COMPONENT_SOFTFP/TOOLCHAIN_GCC_ARM/libcy_capsense.a)
