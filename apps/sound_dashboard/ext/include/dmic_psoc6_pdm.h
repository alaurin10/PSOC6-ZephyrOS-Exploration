/*
 * Extras of the PSoC 6 PDM DMIC driver beyond the standard DMIC API.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DMIC_PSOC6_PDM_H_
#define DMIC_PSOC6_PDM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

struct dmic_psoc6_pdm_stats {
	bool dma;               /* samples moved by DMA (else by the FIFO interrupt) */
	uint32_t pdm_clk_hz;    /* PDM clock on the microphone pin */
	uint32_t rate_hz;       /* PCM sample rate the clock dividers give */
	uint32_t blocks;        /* blocks handed to the reader since the last start */
	uint32_t dropped;       /* times samples were lost: no free block, or the reader fell behind */
	uint32_t fifo_overflows;/* hardware FIFO overflowed: samples lost before the driver */
	uint32_t irqs;          /* PDM or DMA interrupts since the last start */
	uint32_t dma_errors;    /* DataWire error interrupts */
	/* Measured rate: frames delivered between the first and the latest block. */
	uint64_t frames;
	int64_t first_block_ticks;
	int64_t last_block_ticks;
};

/** Copy the driver's counters for the current (or last) stream. */
int dmic_psoc6_pdm_get_stats(const struct device *dev, struct dmic_psoc6_pdm_stats *stats);

#endif /* DMIC_PSOC6_PDM_H_ */
