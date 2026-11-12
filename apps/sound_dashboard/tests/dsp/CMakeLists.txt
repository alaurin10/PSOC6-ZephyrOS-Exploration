# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the audio analysis in src/audio_dsp.c. They need no
# hardware: run them on the board, or in QEMU (also as a Cortex-M0, like the
# PSoC 6's second core).

cmake_minimum_required(VERSION 3.20.0)

set(DASHBOARD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${DASHBOARD_DIR}/cmake/fetch_modules.cmake)
list(APPEND ZEPHYR_EXTRA_MODULES ${CMSIS_DSP_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sound_dashboard_dsp_tests)

target_sources(app PRIVATE src/main.c ${DASHBOARD_DIR}/src/audio_dsp.c)
target_include_directories(app PRIVATE ${DASHBOARD_DIR}/src)
