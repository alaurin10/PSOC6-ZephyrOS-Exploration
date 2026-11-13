# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Third-party modules the west manifest doesn't include (CapSense, CMSIS-DSP,
# LittleFS): fetched once into <workspace>/modules, see cmake/fetch_modules.cmake.
include(cmake/fetch_modules.cmake)

# Extra Zephyr modules: ext/ holds this app's own drivers, devicetree bindings
# (and later the CM0+ SoC/board port). They must be listed before find_package().
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/ext
  ${CMSIS_DSP_DIR}
  ${LITTLEFS_DIR}
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sound_dashboard)

target_sources(app PRIVATE
  src/main.c
  src/channels.c
  src/bus_monitor.c
  src/thermistor.c
  src/app_settings.c
  src/app_watchdog.c
  src/capsense_input.c
  src/ui_ctrl.c
  src/audio_capture.c
  src/audio_dsp.c
  src/app_time.c
  src/wifi_mgr.c
  src/web_server.c
  src/datalog.c
)

# ---- Web dashboard ------------------------------------------------------------
# The page, script and stylesheet are gzipped at build time into C arrays that
# the HTTP server serves as-is (Content-Encoding: gzip).
set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(web_file index.html app.js style.css)
  generate_inc_file_for_target(app web/${web_file} ${gen_dir}/${web_file}.gz.inc --gzip)
endforeach()

# The HTTP server finds the service's resources in an iterable linker section.
zephyr_linker_sources(SECTIONS sections-rom.ld)
zephyr_linker_section(NAME http_resource_desc_dash_service
  KVMA RAM_REGION GROUP RODATA_REGION)

# ---- CapSense -----------------------------------------------------------------
# Infineon CapSense middleware (Infineon EULA, not committed), plus the
# generated configuration for this kit in capsense/.
file(GLOB CAPSENSE_SOURCES ${CAPSENSE_DIR}/*.c)
target_sources(app PRIVATE
  ${CAPSENSE_SOURCES}
  capsense/cycfg_capsense.c
  # PDL driver for the CSD hardware block; the HAL module doesn't build it.
  ${ZEPHYR_HAL_INFINEON_MODULE_DIR}/mtb-pdl-cat1/drivers/source/cy_csd.c
)
target_include_directories(app PRIVATE capsense ${CAPSENSE_DIR})

# Closed-source parts (SmartSense auto-tuning, advanced filters), built for the
# same floating-point ABI as the rest of the image.
if(CONFIG_FP_HARDABI)
  set(CAPSENSE_FP COMPONENT_HARDFP)
else()
  set(CAPSENSE_FP COMPONENT_SOFTFP)
endif()
zephyr_link_libraries(${CAPSENSE_DIR}/${CAPSENSE_FP}/TOOLCHAIN_GCC_ARM/libcy_capsense.a)
