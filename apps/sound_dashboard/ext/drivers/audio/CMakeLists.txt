# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(dmic_psoc6_pdm.c)
# The HAL module only builds the PDM-PCM v2 PDL driver (PSOC Edge), not the
# v1 one this block needs.
zephyr_library_sources(${ZEPHYR_HAL_INFINEON_MODULE_DIR}/mtb-pdl-cat1/drivers/source/cy_pdm_pcm.c)
