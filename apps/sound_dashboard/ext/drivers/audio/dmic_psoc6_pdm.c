/*
 * PSoC 6 (CAT1A) PDM-to-PCM converter as a Zephyr DMIC driver.
 *
 * The audio subsystem's PDM-PCM block turns the 1-bit PDM stream from up to
 * two microphones on one data line into 16- or 24-bit PCM, with its own
 * decimation (sinc) filter, DC-blocking high-pass filter and gain, and puts
 * the samples into a 254-word receive FIFO. This driver moves them from the
 * FIFO into blocks allocated from the application's memory slab, and hands
 * full blocks to dmic_read() through a message queue:
 *
 *   FIFO interrupt mode: the FIFO interrupts when it holds 128 words and the
 *   ISR copies them into the current block.
 *
 *   DMA mode (CONFIG_DMIC_PSOC6_PDM_DMA): DataWire 1 channel 26, which the
 *   PDM's DMA request drives directly, runs an endless chain of two
 *   descriptors. Each one fills one slab block, a burst of samples per
 *   request. When one finishes, the other keeps the stream going while the
 *   ISR passes the full block on and points the finished descriptor at a
 *   fresh block (zero copy, one interrupt per block).
 *
 * The PDL's PDM-PCM v1 driver (cy_pdm_pcm.c) does the register work; Zephyr's
 * own "infineon,pdm" driver is for the v2 block on newer parts.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT infineon_cat1a_pdm

#include <errno.h>
#include <stdlib.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <cy_pdm_pcm.h>
#include <cy_sysclk.h>
#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
#include <cy_dma.h>
#include <cy_trigmux.h>
#endif

#include <dmic_psoc6_pdm.h>

LOG_MODULE_REGISTER(dmic_psoc6_pdm, CONFIG_AUDIO_DMIC_LOG_LEVEL);

/* The audio subsystem is hard-wired to CLK_HF1 on CAT1A. */
#define PDM_HF_CLOCK        1
#define PDM_RATE_MIN        8000
#define PDM_RATE_MAX        48000
#define PDM_FIFO_HALF       128
/* Interrupt mode: interrupt when the FIFO holds more than this many words. */
#define PDM_IRQ_FIFO_LEVEL  (PDM_FIFO_HALF - 1)
#define PDM_GAIN_0DB        CY_PDM_PCM_BYPASS
#define PDM_GAIN_MAX        CY_PDM_PCM_GAIN_10_5_DB
#define DMA_LOOP_MAX        256

struct pdm_config {
	PDM_Type *base;
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config)(void);
	uint32_t irq;
	uint8_t decimation;
	uint8_t high_pass_gain;
	uint8_t cko_delay;
#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
	DW_Type *dw;
	uint32_t dw_channel;
	uint32_t dw_irq;
#endif
};

struct pdm_data {
	enum dmic_state state;
	struct k_mem_slab *slab;
	size_t block_size;          /* bytes */
	uint32_t block_frames;
	uint8_t sample_bytes;       /* 2 (16-bit PCM) or 4 (32-bit PCM) */
	uint8_t channels;
	struct k_msgq rx_queue;
	void *rx_msgs[CONFIG_DMIC_PSOC6_PDM_QUEUE_SIZE];

	/* FIFO interrupt mode: the block the ISR is filling. */
	uint8_t *cur;
	size_t fill;
	uint32_t word_idx;          /* FIFO words read; keeps stereo frames aligned */
	bool dropping;              /* no free block: samples are being discarded */

#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
	cy_stc_dma_descriptor_t desc[2];
	void *dma_buf[2];           /* block each descriptor writes into */
	uint8_t dma_next;           /* descriptor that completes next */
	uint16_t dma_burst;         /* samples moved per DMA request */
#endif

	struct dmic_psoc6_pdm_stats stats;
};

/* ---- Delivering blocks --------------------------------------------------- */

/* Pass a full block to the reader (ISR context). Frees it if the reader is
 * too far behind, so the stream itself never stalls.
 */
static void deliver_block(struct pdm_data *data, void *block)
{
	if (k_msgq_put(&data->rx_queue, &block, K_NO_WAIT) != 0) {
		k_mem_slab_free(data->slab, block);
		data->stats.dropped++;
		return;
	}

	int64_t now = k_uptime_ticks();

	if (data->stats.blocks++ == 0) {
		data->stats.first_block_ticks = now;
	} else {
		data->stats.frames += data->block_frames;
	}
	data->stats.last_block_ticks = now;
}

static void flush_rx_queue(struct pdm_data *data)
{
	void *block;

	while (k_msgq_get(&data->rx_queue, &block, K_NO_WAIT) == 0) {
		k_mem_slab_free(data->slab, block);
	}
}

/* ---- FIFO interrupt mode ---------------------------------------------------- */

static inline void store_sample(struct pdm_data *data, uint32_t word)
{
	if (data->sample_bytes == 2) {
		/* 16-bit word length, sign-extended by the hardware. */
		*(int16_t *)(data->cur + data->fill) = (int16_t)word;
	} else {
		/* 24-bit samples, sign-extended to 32: scale to full 32-bit range. */
		*(int32_t *)(data->cur + data->fill) = (int32_t)(word << 8);
	}
	data->fill += data->sample_bytes;
}

static void drain_fifo(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	uint32_t words = Cy_PDM_PCM_GetNumInFifo(cfg->base);

	while (words-- > 0) {
		uint32_t word = Cy_PDM_PCM_ReadFifo(cfg->base);
		/* In stereo the FIFO alternates left and right: only ever start a
		 * block on a left sample, even after dropping samples.
		 */
		bool frame_start = data->channels == 1 || (data->word_idx & 1) == 0;

		data->word_idx++;
		if (data->cur == NULL) {
			if (!frame_start) {
				continue;
			}
			if (k_mem_slab_alloc(data->slab, (void **)&data->cur, K_NO_WAIT) != 0) {
				/* Reader holds every block: drop samples until one is
				 * free, counting each run of losses once.
				 */
				data->cur = NULL;
				if (!data->dropping) {
					data->dropping = true;
					data->stats.dropped++;
				}
				continue;
			}
			data->dropping = false;
			data->fill = 0;
		}

		store_sample(data, word);
		if (data->fill == data->block_size) {
			deliver_block(data, data->cur);
			data->cur = NULL;
			data->fill = 0;
		}
	}
}

static void pdm_isr(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	uint32_t status = Cy_PDM_PCM_GetInterruptStatusMasked(cfg->base);

	data->stats.irqs++;
	if (status & CY_PDM_PCM_INTR_RX_OVERFLOW) {
		data->stats.fifo_overflows++;
	}
	if (status & CY_PDM_PCM_INTR_RX_TRIGGER) {
		drain_fifo(dev);
	}
	/* Clear after draining: the trigger condition is gone by now. */
	Cy_PDM_PCM_ClearInterrupt(cfg->base, status);
}

/* ---- DMA mode -------------------------------------------------------------- */

#ifdef CONFIG_DMIC_PSOC6_PDM_DMA

/* Largest burst (samples per DMA request) that splits a block evenly, keeps
 * whole frames together, leaves FIFO headroom and fits the DataWire's
 * 256 x 256 2D loop. 0 if there's none.
 */
static uint16_t pick_dma_burst(uint32_t block_samples, uint8_t channels)
{
	for (uint32_t burst = PDM_FIFO_HALF; burst >= channels; burst--) {
		if (block_samples % burst == 0 && burst % channels == 0 &&
		    block_samples / burst <= DMA_LOOP_MAX) {
			return (uint16_t)burst;
		}
	}
	return 0;
}

static void init_descriptor(const struct device *dev, int i)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	uint32_t block_samples = data->block_size / data->sample_bytes;
	const cy_stc_dma_descriptor_config_t dcfg = {
		/* The PDM request is a level signal: give it time to drop after a
		 * burst, or the channel would read an empty FIFO.
		 */
		.retrigger = CY_DMA_RETRIG_16CYC,
		.interruptType = CY_DMA_DESCR,       /* interrupt when the block is full */
		.triggerOutType = CY_DMA_1ELEMENT,   /* unused */
		.channelState = CY_DMA_CHANNEL_ENABLED,
		.triggerInType = CY_DMA_X_LOOP,      /* one request moves one burst */
		.dataSize = data->sample_bytes == 2 ? CY_DMA_HALFWORD : CY_DMA_WORD,
		.srcTransferSize = CY_DMA_TRANSFER_SIZE_WORD, /* the FIFO is a 32-bit register */
		.dstTransferSize = CY_DMA_TRANSFER_SIZE_DATA,
		.descriptorType = CY_DMA_2D_TRANSFER,
		.srcAddress = (void *)&PDM_PCM_RX_FIFO_RD(cfg->base),
		.dstAddress = data->dma_buf[i],
		.srcXincrement = 0,
		.dstXincrement = 1,
		.xCount = data->dma_burst,
		.srcYincrement = 0,
		.dstYincrement = data->dma_burst,
		.yCount = block_samples / data->dma_burst,
		.nextDescriptor = &data->desc[i ^ 1],
	};

	(void)Cy_DMA_Descriptor_Init(&data->desc[i], &dcfg);
}

static void pdm_dma_isr(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	cy_en_dma_intr_cause_t cause = Cy_DMA_Channel_GetStatus(cfg->dw, cfg->dw_channel);

	Cy_DMA_Channel_ClearInterrupt(cfg->dw, cfg->dw_channel);
	data->stats.irqs++;
	if (cause != CY_DMA_INTR_CAUSE_COMPLETION) {
		data->stats.dma_errors++;
		return;
	}

	/* Descriptor i just filled its block; the channel has moved on to the
	 * other one, so descriptor i can be pointed at a new block until that
	 * one finishes too (one whole block of time). Ask the hardware which
	 * one is active rather than toggling: if this interrupt was held off
	 * for longer than a block (e.g. the CPU stalled on a flash write), a
	 * completion was missed and the toggle would point at the wrong one.
	 */
	const cy_stc_dma_descriptor_t *active =
		Cy_DMA_Channel_GetCurrentDescriptor(cfg->dw, cfg->dw_channel);
	int i = active == &data->desc[0] ? 1 : 0;
	void *done = data->dma_buf[i];
	void *next;

	if (i != data->dma_next) {
		/* That block was filled twice; the first fill is lost. */
		data->stats.dropped++;
	}
	data->dma_next = i ^ 1;
	if (k_mem_slab_alloc(data->slab, &next, K_NO_WAIT) == 0) {
		deliver_block(data, done);
		data->dma_buf[i] = next;
		Cy_DMA_Descriptor_SetDstAddress(&data->desc[i], next);
	} else {
		/* No free block: refill the same one, its samples are lost. */
		data->stats.dropped++;
	}
}

static int dma_start(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;

	for (int i = 0; i < 2; i++) {
		if (k_mem_slab_alloc(data->slab, &data->dma_buf[i], K_NO_WAIT) != 0) {
			if (i == 1) {
				k_mem_slab_free(data->slab, data->dma_buf[0]);
			}
			return -ENOMEM;
		}
		init_descriptor(dev, i);
	}
	data->dma_next = 0;

	const cy_stc_dma_channel_config_t ch_cfg = {
		.descriptor = &data->desc[0],
		.preemptable = false,
		.priority = 0, /* highest: audio can't wait */
		.enable = false,
		.bufferable = false,
	};

	if (Cy_DMA_Channel_Init(cfg->dw, cfg->dw_channel, &ch_cfg) != CY_DMA_SUCCESS) {
		k_mem_slab_free(data->slab, data->dma_buf[0]);
		k_mem_slab_free(data->slab, data->dma_buf[1]);
		return -EIO;
	}
	Cy_DMA_Channel_ClearInterrupt(cfg->dw, cfg->dw_channel);
	Cy_DMA_Channel_SetInterruptMask(cfg->dw, cfg->dw_channel, CY_DMA_INTR_MASK);
	irq_enable(cfg->dw_irq);
	Cy_DMA_Enable(cfg->dw);
	Cy_DMA_Channel_Enable(cfg->dw, cfg->dw_channel);
	return 0;
}

static void dma_stop(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;

	Cy_DMA_Channel_Disable(cfg->dw, cfg->dw_channel);
	irq_disable(cfg->dw_irq);
	Cy_DMA_Channel_SetInterruptMask(cfg->dw, cfg->dw_channel, 0);
	Cy_DMA_Channel_ClearInterrupt(cfg->dw, cfg->dw_channel);
	for (int i = 0; i < 2; i++) {
		k_mem_slab_free(data->slab, data->dma_buf[i]);
		data->dma_buf[i] = NULL;
	}
}

#endif /* CONFIG_DMIC_PSOC6_PDM_DMA */

/* ---- DMIC API ------------------------------------------------------------- */

/* Split HF1 into the PDM clock with the block's three dividers (two of 1..4,
 * one of 2..16), closest match to `target`. Same search as the legacy HAL.
 */
static uint32_t pick_dividers(uint32_t hf_hz, uint32_t target, cy_stc_pdm_pcm_config_t *pc)
{
	uint32_t best_err = UINT32_MAX;
	uint32_t best_hz = 0;

	for (uint32_t d1 = 1; d1 <= 4; d1++) {
		for (uint32_t d0 = d1; d0 <= 4; d0++) {
			for (uint32_t d2 = 2; d2 <= 16; d2++) {
				uint32_t hz = hf_hz / (d0 * d1 * d2);
				uint32_t err = hz > target ? hz - target : target - hz;

				if (err < best_err) {
					best_err = err;
					best_hz = hz;
					pc->clkDiv = (cy_en_pdm_pcm_clk_div_t)(d0 - 1);
					pc->mclkDiv = (cy_en_pdm_pcm_clk_div_t)(d1 - 1);
					pc->ckoDiv = (uint8_t)(d2 - 1);
				}
			}
		}
	}
	return best_hz;
}

static int pdm_configure(const struct device *dev, struct dmic_cfg *config)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	struct pdm_chan_cfg *chan = &config->channel;
	struct pcm_stream_cfg *stream = &config->streams[0];
	const uint32_t map_l = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	const uint32_t map_r = dmic_build_channel_map(0, 0, PDM_CHAN_RIGHT);
	const uint32_t map_lr = map_l | dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
	const uint32_t map_rl = map_r | dmic_build_channel_map(1, 0, PDM_CHAN_LEFT);
	cy_stc_pdm_pcm_config_t pc = {
		.ckoDelay = cfg->cko_delay,
		.sincDecRate = cfg->decimation / 2,
		.highPassFilterGain = cfg->high_pass_gain,
		.highPassDisable = false,
		.softMuteCycles = CY_PDM_PCM_SOFT_MUTE_CYCLES_96,
		.softMuteFineGain = 1,
		.softMuteEnable = false,
		.signExtension = true,
	};

	if (data->state == DMIC_STATE_ACTIVE) {
		return -EBUSY;
	}
	if (chan->req_num_streams != 1 || stream->mem_slab == NULL || stream->block_size == 0) {
		LOG_ERR("Need exactly one stream with a memory slab");
		return -EINVAL;
	}
	if (stream->pcm_rate < PDM_RATE_MIN || stream->pcm_rate > PDM_RATE_MAX) {
		LOG_ERR("Sample rate %u Hz not in %u..%u", stream->pcm_rate, PDM_RATE_MIN,
			PDM_RATE_MAX);
		return -EINVAL;
	}
	if (stream->pcm_width != 16 && stream->pcm_width != 32) {
		LOG_ERR("PCM width must be 16 or 32 (24-bit samples, left-justified)");
		return -EINVAL;
	}

	/* One PDM line, left and right microphone: mono L, mono R, or stereo
	 * in either order (the hardware can swap the pair).
	 */
	if (chan->req_num_chan == 1 && chan->req_chan_map_lo == map_l) {
		pc.chanSelect = CY_PDM_PCM_OUT_CHAN_LEFT;
	} else if (chan->req_num_chan == 1 && chan->req_chan_map_lo == map_r) {
		pc.chanSelect = CY_PDM_PCM_OUT_CHAN_RIGHT;
	} else if (chan->req_num_chan == 2 &&
		   (chan->req_chan_map_lo == map_lr || chan->req_chan_map_lo == map_rl)) {
		pc.chanSelect = CY_PDM_PCM_OUT_STEREO;
		pc.chanSwapEnable = chan->req_chan_map_lo == map_rl;
	} else {
		LOG_ERR("Unsupported channel map 0x%08x (%u channels)", chan->req_chan_map_lo,
			chan->req_num_chan);
		return -EINVAL;
	}

	uint8_t sample_bytes = stream->pcm_width / 8;
	uint32_t frame_bytes = sample_bytes * chan->req_num_chan;

	if (stream->block_size % frame_bytes != 0 ||
	    stream->block_size > stream->mem_slab->info.block_size) {
		LOG_ERR("Block size %u must be whole frames and fit the slab", stream->block_size);
		return -EINVAL;
	}

	/* Clock: PDM clock = sample rate x decimation, divided down from HF1. */
	uint32_t hf_hz = Cy_SysClk_ClkHfGetFrequency(PDM_HF_CLOCK);
	uint32_t want_hz = stream->pcm_rate * cfg->decimation;
	uint32_t pdm_hz = pick_dividers(hf_hz, want_hz, &pc);

	if (hf_hz == 0 || pdm_hz == 0) {
		LOG_ERR("CLK_HF%d is off", PDM_HF_CLOCK);
		return -EIO;
	}
	if ((config->io.min_pdm_clk_freq && pdm_hz < config->io.min_pdm_clk_freq) ||
	    (config->io.max_pdm_clk_freq && pdm_hz > config->io.max_pdm_clk_freq)) {
		LOG_ERR("PDM clock %u Hz outside the requested %u..%u", pdm_hz,
			config->io.min_pdm_clk_freq, config->io.max_pdm_clk_freq);
		return -EINVAL;
	}
	if ((uint64_t)abs((int32_t)(pdm_hz - want_hz)) * 100 > want_hz) {
		LOG_WRN("PDM clock %u Hz is more than 1%% off %u Hz (CLK_HF%d %u Hz)", pdm_hz,
			want_hz, PDM_HF_CLOCK, hf_hz);
	}

	/* Gain in 1.5 dB steps from -12 to +10.5 dB. */
	int gain = PDM_GAIN_0DB + DIV_ROUND_CLOSEST(stream->gain_db * 2, 3);

	pc.gainLeft = pc.gainRight = (cy_en_pdm_pcm_gain_t)CLAMP(gain, 0, PDM_GAIN_MAX);
	pc.wordLen = sample_bytes == 2 ? CY_PDM_PCM_WLEN_16_BIT : CY_PDM_PCM_WLEN_24_BIT;

#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
	uint16_t burst = pick_dma_burst(stream->block_size / sample_bytes, chan->req_num_chan);

	if (burst == 0) {
		LOG_ERR("Block of %u samples can't be split into DMA bursts",
			stream->block_size / sample_bytes);
		return -EINVAL;
	}
	data->dma_burst = burst;
	/* Request the DMA when the FIFO holds a whole burst. */
	pc.rxFifoTriggerLevel = burst - 1;
	pc.dmaTriggerEnable = true;
	pc.interruptMask = CY_PDM_PCM_INTR_RX_OVERFLOW;
#else
	pc.rxFifoTriggerLevel = PDM_IRQ_FIFO_LEVEL;
	pc.dmaTriggerEnable = false;
	pc.interruptMask = CY_PDM_PCM_INTR_RX_TRIGGER | CY_PDM_PCM_INTR_RX_OVERFLOW;
#endif

	/* Writes the configuration and enables the block; streaming starts later. */
	Cy_PDM_PCM_Disable(cfg->base);
	if (Cy_PDM_PCM_Init(cfg->base, &pc) != CY_PDM_PCM_SUCCESS) {
		return -EIO;
	}
	Cy_PDM_PCM_SetInterruptMask(cfg->base, 0); /* enabled on start */

	data->slab = stream->mem_slab;
	data->block_size = stream->block_size;
	data->block_frames = stream->block_size / frame_bytes;
	data->sample_bytes = sample_bytes;
	data->channels = chan->req_num_chan;
	data->stats.dma = IS_ENABLED(CONFIG_DMIC_PSOC6_PDM_DMA);
	data->stats.pdm_clk_hz = pdm_hz;
	data->stats.rate_hz = pdm_hz / cfg->decimation;

	chan->act_num_chan = chan->req_num_chan;
	chan->act_chan_map_lo = chan->req_chan_map_lo;
	chan->act_chan_map_hi = 0;
	chan->act_num_streams = 1;
	data->state = DMIC_STATE_CONFIGURED;

	LOG_INF("%u ch, %u-bit, %u Hz (PDM clock %u Hz from %u Hz), %u-byte blocks, %s",
		data->channels, stream->pcm_width, data->stats.rate_hz, pdm_hz, hf_hz,
		data->block_size, data->stats.dma ? "DMA" : "FIFO interrupt");
	return 0;
}

static int stream_start(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	struct dmic_psoc6_pdm_stats *st = &data->stats;

	/* Keep the configuration part, restart the counters. */
	st->blocks = st->dropped = st->fifo_overflows = st->irqs = st->dma_errors = 0;
	st->frames = 0;
	data->cur = NULL;
	data->fill = 0;
	data->word_idx = 0;
	data->dropping = false;

	Cy_PDM_PCM_ClearFifo(cfg->base);
	Cy_PDM_PCM_ClearInterrupt(cfg->base, CY_PDM_PCM_INTR_MASK);

#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
	int ret = dma_start(dev);

	if (ret != 0) {
		return ret;
	}
	Cy_PDM_PCM_SetInterruptMask(cfg->base, CY_PDM_PCM_INTR_RX_OVERFLOW);
#else
	Cy_PDM_PCM_SetInterruptMask(cfg->base,
				    CY_PDM_PCM_INTR_RX_TRIGGER | CY_PDM_PCM_INTR_RX_OVERFLOW);
#endif
	irq_enable(cfg->irq);
	Cy_PDM_PCM_Enable(cfg->base);
	return 0;
}

static void stream_stop(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;

	Cy_PDM_PCM_Disable(cfg->base);
	irq_disable(cfg->irq);
	Cy_PDM_PCM_SetInterruptMask(cfg->base, 0);
#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
	dma_stop(dev);
#endif
	if (data->cur != NULL) {
		k_mem_slab_free(data->slab, data->cur);
		data->cur = NULL;
	}
	Cy_PDM_PCM_ClearFifo(cfg->base);
}

static int pdm_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	struct pdm_data *data = dev->data;
	int ret = 0;

	switch (cmd) {
	case DMIC_TRIGGER_START:
	case DMIC_TRIGGER_RELEASE:
		if (data->state == DMIC_STATE_ACTIVE) {
			break;
		}
		if (data->state != DMIC_STATE_CONFIGURED && data->state != DMIC_STATE_PAUSED) {
			return -EIO;
		}
		ret = stream_start(dev);
		if (ret == 0) {
			data->state = DMIC_STATE_ACTIVE;
		}
		break;
	case DMIC_TRIGGER_STOP:
	case DMIC_TRIGGER_PAUSE:
		if (data->state == DMIC_STATE_ACTIVE) {
			stream_stop(dev);
			data->state = cmd == DMIC_TRIGGER_PAUSE ? DMIC_STATE_PAUSED
								: DMIC_STATE_CONFIGURED;
		}
		break;
	case DMIC_TRIGGER_RESET:
		if (data->state == DMIC_STATE_ACTIVE) {
			stream_stop(dev);
		}
		if (data->slab != NULL) {
			flush_rx_queue(data);
		}
		if (data->state != DMIC_STATE_UNINIT && data->state != DMIC_STATE_INITIALIZED) {
			data->state = DMIC_STATE_CONFIGURED;
		}
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int pdm_read(const struct device *dev, uint8_t stream, void **buffer, size_t *size,
		    int32_t timeout)
{
	struct pdm_data *data = dev->data;

	if (stream != 0) {
		return -EINVAL;
	}
	if (data->state == DMIC_STATE_UNINIT || data->state == DMIC_STATE_INITIALIZED) {
		return -EIO;
	}

	/* Blocks left over after a stop can still be read; then it's empty. */
	bool active = data->state == DMIC_STATE_ACTIVE;
	int ret = k_msgq_get(&data->rx_queue, buffer, active ? SYS_TIMEOUT_MS(timeout) : K_NO_WAIT);

	if (ret != 0) {
		return active ? -EAGAIN : -EIO;
	}
	*size = data->block_size;
	return 0;
}

int dmic_psoc6_pdm_get_stats(const struct device *dev, struct dmic_psoc6_pdm_stats *stats)
{
	struct pdm_data *data = dev->data;
	unsigned int key = irq_lock();

	*stats = data->stats;
	irq_unlock(key);
	return 0;
}

static int pdm_init(const struct device *dev)
{
	const struct pdm_config *cfg = dev->config;
	struct pdm_data *data = dev->data;
	int ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);

	if (ret < 0) {
		return ret;
	}

	k_msgq_init(&data->rx_queue, (char *)data->rx_msgs, sizeof(void *),
		    ARRAY_SIZE(data->rx_msgs));
	cfg->irq_config();

#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
	/* The PDM's DMA request has a dedicated line to DataWire 1 channel 26. */
	if (Cy_TrigMux_Select(TRIG_OUT_1TO1_4_PDM0_RX_TO_PDMA1_TR_IN26, false,
			      TRIGGER_TYPE_AUDIOSS_TR_PDM_RX_REQ) != CY_TRIGMUX_SUCCESS) {
		return -EIO;
	}
#endif

	data->state = DMIC_STATE_INITIALIZED;
	return 0;
}

static DEVICE_API(dmic, pdm_api) = {
	.configure = pdm_configure,
	.trigger = pdm_trigger,
	.read = pdm_read,
};

#ifdef CONFIG_DMIC_PSOC6_PDM_DMA
#define PDM_DMA_NODE(n) DT_INST_DMAS_CTLR_BY_NAME(n, rx)
#define PDM_DMA_CH(n)   DT_INST_DMAS_CELL_BY_NAME(n, rx, channel)

#define PDM_DMA_IRQ_CONNECT(n)                                                                     \
	IRQ_CONNECT(DT_IRQ_BY_IDX(PDM_DMA_NODE(n), PDM_DMA_CH(n), irq),                            \
		    DT_IRQ_BY_IDX(PDM_DMA_NODE(n), PDM_DMA_CH(n), priority), pdm_dma_isr,          \
		    DEVICE_DT_INST_GET(n), 0);

#define PDM_DMA_CONFIG(n)                                                                          \
	.dw = (DW_Type *)DT_REG_ADDR(PDM_DMA_NODE(n)),                                             \
	.dw_channel = PDM_DMA_CH(n),                                                               \
	.dw_irq = DT_IRQ_BY_IDX(PDM_DMA_NODE(n), PDM_DMA_CH(n), irq),

#define PDM_DMA_CHECK(n)                                                                           \
	BUILD_ASSERT(DT_REG_ADDR(PDM_DMA_NODE(n)) == DW1_BASE && PDM_DMA_CH(n) == 26,              \
		     "the PDM's DMA request is wired to DataWire 1 channel 26");                   \
	BUILD_ASSERT(!DT_NODE_HAS_STATUS_OKAY(PDM_DMA_NODE(n)),                                    \
		     "keep the dma1 node disabled: this driver owns DataWire 1 channel 26");
#else
#define PDM_DMA_IRQ_CONNECT(n)
#define PDM_DMA_CONFIG(n)
#define PDM_DMA_CHECK(n)
#endif

#define PDM_INIT(n)                                                                                \
	BUILD_ASSERT(DT_SAME_NODE(DT_INST_CLOCKS_CTLR(n), DT_NODELABEL(clk_hf1)),                  \
		     "the PSoC 6 audio subsystem is clocked from clk_hf1");                        \
	BUILD_ASSERT(DT_INST_PROP(n, decimation_rate) % 2 == 0 &&                                  \
		     DT_INST_PROP(n, decimation_rate) >= 16 &&                                     \
		     DT_INST_PROP(n, decimation_rate) <= 254, "decimation-rate: even, 16..254");   \
	BUILD_ASSERT(DT_INST_PROP(n, high_pass_gain) <= 15, "high-pass-gain: 0..15");              \
	PDM_DMA_CHECK(n)                                                                           \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	static void pdm_irq_config_##n(void)                                                       \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), pdm_isr,                    \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		PDM_DMA_IRQ_CONNECT(n)                                                             \
	}                                                                                          \
                                                                                                   \
	static const struct pdm_config pdm_config_##n = {                                          \
		.base = (PDM_Type *)DT_INST_REG_ADDR(n),                                           \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                         \
		.irq_config = pdm_irq_config_##n,                                                  \
		.irq = DT_INST_IRQN(n),                                                            \
		.decimation = DT_INST_PROP(n, decimation_rate),                                    \
		.high_pass_gain = DT_INST_PROP(n, high_pass_gain),                                 \
		.cko_delay = DT_INST_PROP(n, cko_delay),                                           \
		PDM_DMA_CONFIG(n)};                                                                \
                                                                                                   \
	static struct pdm_data pdm_data_##n;                                                       \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, pdm_init, NULL, &pdm_data_##n, &pdm_config_##n, POST_KERNEL,      \
			      CONFIG_AUDIO_DMIC_INIT_PRIORITY, &pdm_api);

DT_INST_FOREACH_STATUS_OKAY(PDM_INIT)
