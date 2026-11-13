# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_AUDIO_DMIC_PSOC6_PDM audio)
add_subdirectory_ifdef(CONFIG_FLASH_PSOC6_SMIF flash)
