# SPDX-License-Identifier: Apache-2.0
#
# Third-party code this app needs that the west manifest (ifx-zephyr-sdk, which
# only imports an allow-list of Zephyr's modules) doesn't fetch. Each one is
# cloned once, at a pinned revision, into the workspace's modules/ folder next
# to the west-managed code. Delete a folder to fetch it again.
#
# Sets: CAPSENSE_DIR, CMSIS_DSP_DIR, LITTLEFS_DIR

# The repository root is also the west workspace root (where .west/ lives).
get_filename_component(WEST_WORKSPACE ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)

# fetch_pinned(<name> <url> <rev> <dir> <marker>): shallow-fetch one revision
# (tag, branch or commit SHA) unless <dir>/<marker> already exists.
function(fetch_pinned name url rev dir marker)
  if(EXISTS ${dir}/${marker})
    return()
  endif()

  message(STATUS "Fetching ${name} ${rev} into ${dir}")
  file(REMOVE_RECURSE ${dir})
  file(MAKE_DIRECTORY ${dir})
  foreach(step "init -q" "remote add origin ${url}" "fetch -q --depth 1 origin ${rev}"
               "checkout -q FETCH_HEAD")
    separate_arguments(git_args UNIX_COMMAND "${step}")
    execute_process(COMMAND git ${git_args} WORKING_DIRECTORY ${dir} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      file(REMOVE_RECURSE ${dir})
      message(FATAL_ERROR "Fetching ${name} failed at 'git ${step}'")
    endif()
  endforeach()
endfunction()

# Infineon CapSense middleware (Infineon EULA). Same folder as rtos_blinky uses.
set(CAPSENSE_DIR ${WEST_WORKSPACE}/modules/lib/capsense)
fetch_pinned(CapSense https://github.com/Infineon/capsense release-v3.0.1
             ${CAPSENSE_DIR} cy_capsense.h)

# Zephyr's forks of CMSIS-DSP and LittleFS, at the revisions pinned by this
# Zephyr tree's own west.yml (ifx-zephyr/west.yml).
set(CMSIS_DSP_DIR ${WEST_WORKSPACE}/modules/lib/cmsis-dsp)
fetch_pinned(CMSIS-DSP https://github.com/zephyrproject-rtos/cmsis-dsp
             45d23287f5b41be49612281a5e7c802c6a415388 ${CMSIS_DSP_DIR} zephyr/module.yml)

set(LITTLEFS_DIR ${WEST_WORKSPACE}/modules/fs/littlefs)
fetch_pinned(LittleFS https://github.com/zephyrproject-rtos/littlefs
             e9a8638fc228e43a3156ed3ac602229af3e3c1d5 ${LITTLEFS_DIR} zephyr/module.yml)
