/*
 * Audio capture and analysis: owns the microphone stream.
 *
 * Configures the PDM microphone through the standard DMIC API (the PSoC 6
 * driver lives in ext/drivers/audio). A high-priority thread reads one block
 * at a time (zero copy, straight from the DMA) and runs the analysis in
 * audio_dsp.c on it:
 *
 *   every block      level, FFT (1024 points, sliding by one block), claps
 *   every 2nd block  publish audio_level_chan and audio_spectrum_chan (~16 Hz)
 *   on a clap/clip   publish audio_event_chan
 *
 * Display gain, smoothing and clap sensitivity come from ui_chan.
 *
 * Shell: "audio" (levels, spectrum, timing), "mic" (stream: start|stop,
 * level, stats, dump).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <dmic_psoc6_pdm.h>

#include "app_watchdog.h"
#include "audio_capture.h"
#include "audio_dsp.h"
#include "channels.h"

LOG_MODULE_REGISTER(audio_capture, LOG_LEVEL_INF);

#define CAP_PRIORITY      3
#define CAP_STACK_SIZE    2048
#define RATE              CONFIG_APP_AUDIO_SAMPLE_RATE
#define BLOCK_SAMPLES     CONFIG_APP_AUDIO_BLOCK_SAMPLES
#define BLOCK_BYTES       (BLOCK_SAMPLES * sizeof(int16_t))
#define PUBLISH_EVERY     2       /* blocks per published level/spectrum */
#define READ_TIMEOUT_MS   (APP_WDT_CHECK_MS / 2)
#define DUMP_MAX          4096
#define CLIP_EVENT_MS     1000    /* at most one clip event per second */
/* After a (re)start the microphone wakes up and the converter's filters
 * settle: the first ~100 ms look like a burst (a clap), so skip them.
 */
#define SETTLE_BLOCKS     DIV_ROUND_UP(RATE / 10, BLOCK_SAMPLES)
#define GAIN_DB_PER_STEP  0.6f    /* UI gain 0..100 -> -30..+30 dB */
#define SMOOTHING_MAX     0.9f    /* UI smoothing 100 -> 0.9 (time constant ~0.6 s) */

BUILD_ASSERT((BLOCK_SAMPLES & (BLOCK_SAMPLES - 1)) == 0, "block size must be a power of two");
BUILD_ASSERT(BLOCK_SAMPLES <= DSP_FFT_SIZE, "a block must fit the FFT window");
BUILD_ASSERT(AUDIO_BANDS == DSP_BANDS);

K_MEM_SLAB_DEFINE_STATIC(audio_slab, BLOCK_BYTES, CONFIG_APP_AUDIO_BLOCK_COUNT, 4);

static const struct device *const dmic = DEVICE_DT_GET(DT_ALIAS(dmic0));

static K_SEM_DEFINE(run_sem, 0, 1);
static atomic_t want_running = ATOMIC_INIT(1);
static atomic_t running;
/* Which microphone on the PDM line: the "mic start" argument, Kconfig default. */
static atomic_t want_right = ATOMIC_INIT(IS_ENABLED(CONFIG_APP_AUDIO_MIC_RIGHT));
static atomic_t cur_right = ATOMIC_INIT(-1);

/* Analysis state: only touched by the capture thread. */
static struct dsp_spectrum spectrum;
static struct dsp_clap clap;
static struct {
	float power_sum;      /* sum of per-block mean-square ratios */
	float peak_dbfs;
	bool clipped;
	uint32_t blocks;
} level_acc = {.peak_dbfs = DSP_DB_MIN};
static uint32_t level_seq, spectrum_seq, event_seq;
static int64_t last_clip_ms = -CLIP_EVENT_MS;

/* Counters and timing for the shell. */
static atomic_t blocks_seen;
static atomic_t claps;
static atomic_t dsp_us_last;
static atomic_t dsp_us_max;
static uint64_t dsp_us_total;

/* Where the analysis time goes, summed over all blocks (us). */
enum { STAGE_LEVEL, STAGE_FFT, STAGE_CLAP, STAGE_PUBLISH, STAGE_COUNT };
static const char *const stage_names[STAGE_COUNT] = {"level", "FFT+bands", "claps", "publish"};
static uint64_t stage_us[STAGE_COUNT];

static inline void stage_end(int stage, uint32_t *t)
{
	uint32_t now = k_cycle_get_32();

	stage_us[stage] += k_cyc_to_us_floor32(now - *t);
	*t = now;
}

/* "mic dump": the capture thread copies samples here for the shell. */
static int16_t dump_buf[DUMP_MAX];
static atomic_t dump_want;   /* samples requested, 0 = none */
static size_t dump_have;
static K_SEM_DEFINE(dump_done, 0, 1);

static void publish_event(uint8_t type, float peak_dbfs)
{
	struct audio_event_msg ev = {
		.seq = ++event_seq,
		.type = type,
		.peak_cdb = (int16_t)lroundf(peak_dbfs * 100.0f),
		.timestamp_ms = k_uptime_get(),
	};

	(void)zbus_chan_pub(&audio_event_chan, &ev, K_MSEC(10));
}

static void publish_level_and_spectrum(const struct ui_msg *ui)
{
	struct audio_level_msg lm = {
		.seq = ++level_seq,
		.rms_cdb = (int16_t)lroundf(
			dsp_db(sqrtf(level_acc.power_sum / (float)level_acc.blocks)) * 100.0f),
		.peak_cdb = (int16_t)lroundf(level_acc.peak_dbfs * 100.0f),
		.clipped = level_acc.clipped,
		.timestamp_ms = k_uptime_get(),
	};
	struct audio_spectrum_msg sm = {
		.seq = ++spectrum_seq,
		.sample_rate_hz = RATE,
		.fft_size = DSP_FFT_SIZE,
		.n_bands = DSP_BANDS,
		.timestamp_ms = lm.timestamp_ms,
	};
	float gain_db = ((int)ui->param[UI_PARAM_GAIN] - UI_PARAM_MAX / 2) * GAIN_DB_PER_STEP;
	float smoothing = ui->param[UI_PARAM_SMOOTHING] * SMOOTHING_MAX / UI_PARAM_MAX;

	dsp_spectrum_take(&spectrum, smoothing, gain_db, AUDIO_BAND_DB_MIN, -AUDIO_BAND_DB_MIN,
			  sm.band_db);

	/* Short timeouts: never hold up the audio for a slow observer. */
	(void)zbus_chan_pub(&audio_level_chan, &lm, K_MSEC(5));
	(void)zbus_chan_pub(&audio_spectrum_chan, &sm, K_MSEC(5));

	level_acc.power_sum = 0.0f;
	level_acc.peak_dbfs = DSP_DB_MIN;
	level_acc.clipped = false;
	level_acc.blocks = 0;
}

static void copy_for_dump(const int16_t *s, size_t n)
{
	size_t want = (size_t)atomic_get(&dump_want);

	if (want == 0) {
		return;
	}

	size_t take = MIN(n, want - dump_have);

	memcpy(&dump_buf[dump_have], s, take * sizeof(int16_t));
	dump_have += take;
	if (dump_have == want) {
		atomic_set(&dump_want, 0);
		k_sem_give(&dump_done);
	}
}

/* ---- Raw recorder (tuning builds, CONFIG_APP_AUDIO_REC_SECONDS > 0) ---------------- */

#if CONFIG_APP_AUDIO_REC_SECONDS > 0
#define REC_SAMPLES (RATE * CONFIG_APP_AUDIO_REC_SECONDS)

enum { REC_EMPTY, REC_RUNNING, REC_DONE };
static int16_t rec_buf[REC_SAMPLES];
static size_t rec_have;
static atomic_t rec_state;
static const struct gpio_dt_spec rec_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void copy_for_rec(const int16_t *s, size_t n)
{
	if (atomic_get(&rec_state) != REC_RUNNING) {
		return;
	}

	size_t take = MIN(n, REC_SAMPLES - rec_have);

	memcpy(&rec_buf[rec_have], s, take * sizeof(int16_t));
	rec_have += take;
	if (rec_have == REC_SAMPLES) {
		(void)gpio_pin_set_dt(&rec_led, 0);
		atomic_set(&rec_state, REC_DONE);
		LOG_INF("Recording done: GET /api/rec.wav");
	}
}

int audio_rec_start(void)
{
	if (atomic_get(&rec_state) == REC_RUNNING) {
		return -EBUSY;
	}
	rec_have = 0;
	/* ui_ctrl also flashes this LED on claps unless "LED on claps" is off. */
	(void)gpio_pin_configure_dt(&rec_led, GPIO_OUTPUT_ACTIVE);
	atomic_set(&rec_state, REC_RUNNING);
	return 0;
}

const int16_t *audio_rec_get(size_t *n_samples)
{
	if (atomic_get(&rec_state) != REC_DONE) {
		return NULL;
	}
	*n_samples = REC_SAMPLES;
	return rec_buf;
}
#else
static inline void copy_for_rec(const int16_t *s, size_t n)
{
	ARG_UNUSED(s);
	ARG_UNUSED(n);
}

int audio_rec_start(void)
{
	return -ENOTSUP;
}

const int16_t *audio_rec_get(size_t *n_samples)
{
	ARG_UNUSED(n_samples);
	return NULL;
}
#endif

static void process_block(const int16_t *s, size_t n)
{
	static struct ui_msg ui = {.param = {UI_PARAM_MAX / 2, 0, UI_PARAM_MAX / 2}};
	uint32_t t0 = k_cycle_get_32();
	uint32_t t = t0;
	struct dsp_level lvl;

	/* The channel always holds the latest UI state; keep the old copy if busy. */
	(void)zbus_chan_read(&ui_chan, &ui, K_NO_WAIT);

	dsp_level_measure(s, n, &lvl);
	level_acc.power_sum += powf(10.0f, lvl.rms_dbfs / 10.0f);
	level_acc.peak_dbfs = MAX(level_acc.peak_dbfs, lvl.peak_dbfs);
	level_acc.clipped |= lvl.clipped;
	level_acc.blocks++;
	stage_end(STAGE_LEVEL, &t);

	dsp_spectrum_push(&spectrum, s, n);
	stage_end(STAGE_FFT, &t);

	enum dsp_clap_event ev = dsp_clap_process(
		&clap, s, n, (float)ui.param[UI_PARAM_CLAP_SENS] / UI_PARAM_MAX);
	stage_end(STAGE_CLAP, &t);

	if (ev != DSP_CLAP_NONE) {
		atomic_inc(&claps);
		publish_event(ev == DSP_CLAP_DOUBLE ? AUDIO_EVT_DOUBLE_CLAP : AUDIO_EVT_CLAP,
			      clap.last_peak_dbfs);
	}
	if (lvl.clipped && k_uptime_get() - last_clip_ms >= CLIP_EVENT_MS) {
		last_clip_ms = k_uptime_get();
		publish_event(AUDIO_EVT_CLIP, 0.0f);
	}

	if (level_acc.blocks == PUBLISH_EVERY) {
		publish_level_and_spectrum(&ui);
	}
	stage_end(STAGE_PUBLISH, &t);

	copy_for_dump(s, n);
	copy_for_rec(s, n);
	atomic_inc(&blocks_seen);

	uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

	atomic_set(&dsp_us_last, us);
	if (us > (uint32_t)atomic_get(&dsp_us_max)) {
		atomic_set(&dsp_us_max, us);
	}
	dsp_us_total += us;
}

static int stream_configure(bool right)
{
	struct pcm_stream_cfg stream = {
		.pcm_rate = RATE,
		.pcm_width = 16,
		.block_size = BLOCK_BYTES,
		.gain_db = CONFIG_APP_AUDIO_GAIN_DB,
		.mem_slab = &audio_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			/* Typical PDM microphone clock range. */
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3250000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
			.req_chan_map_lo = dmic_build_channel_map(
				0, 0, right ? PDM_CHAN_RIGHT : PDM_CHAN_LEFT),
		},
	};

	int ret = dmic_configure(dmic, &cfg);

	if (ret == 0) {
		atomic_set(&cur_right, right);
	}
	return ret;
}

static void capture_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!device_is_ready(dmic)) {
		LOG_ERR("Microphone %s not ready", dmic->name);
		return;
	}
	dsp_clap_init(&clap, RATE);
	app_watchdog_enable(WDT_CLIENT_AUDIO);

	int ret;
	uint32_t settle = 0;

	while (true) {
		app_watchdog_alive(WDT_CLIENT_AUDIO);

		bool switching = atomic_get(&running) &&
				 atomic_get(&want_right) != atomic_get(&cur_right);

		if (!atomic_get(&want_running) || switching) {
			if (atomic_cas(&running, 1, 0)) {
				(void)dmic_trigger(dmic, DMIC_TRIGGER_STOP);
				/* Hand back blocks that were already queued. */
				(void)dmic_trigger(dmic, DMIC_TRIGGER_RESET);
				LOG_INF("Microphone stopped");
			}
			if (!switching) {
				(void)k_sem_take(&run_sem, K_MSEC(READ_TIMEOUT_MS));
			}
			continue;
		}

		if (!atomic_get(&running)) {
			bool right = atomic_get(&want_right);

			ret = right == atomic_get(&cur_right) ? 0 : stream_configure(right);
			if (ret == 0) {
				ret = dmic_trigger(dmic, DMIC_TRIGGER_START);
			}
			if (ret != 0) {
				LOG_ERR("Microphone start failed (%d)", ret);
				atomic_set(&want_running, 0);
				continue;
			}
			atomic_set(&running, 1);
			settle = SETTLE_BLOCKS;
			LOG_INF("Microphone running (%s)", right ? "right" : "left");
		}

		void *buf;
		size_t size;

		ret = dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS);
		if (ret == -EAGAIN) {
			LOG_WRN("No audio for %d ms", READ_TIMEOUT_MS);
			continue;
		}
		if (ret != 0) {
			LOG_ERR("dmic_read failed (%d)", ret);
			continue;
		}

		if (settle > 0) {
			settle--;
		} else {
			process_block(buf, size / sizeof(int16_t));
		}
		k_mem_slab_free(&audio_slab, buf);
	}
}

K_THREAD_DEFINE(audio_cap_tid, CAP_STACK_SIZE, capture_thread, NULL, NULL, NULL, CAP_PRIORITY,
		0, SYS_FOREVER_MS);

void audio_capture_start(void)
{
	/* Set the bands up before the thread starts: other modules ask for their frequencies. */
	if (dsp_spectrum_init(&spectrum, RATE) != 0) {
		LOG_ERR("FFT init failed");
		return;
	}
	k_thread_start(audio_cap_tid);
}

float audio_capture_band_center_hz(int band)
{
	return band >= 0 && band < DSP_BANDS ? dsp_band_center_hz(&spectrum, band) : 0.0f;
}

/* ---- Shell: "audio" and "mic" ------------------------------------------------------ */

static void print_cdb(const struct shell *sh, const char *label, int32_t cdb)
{
	if (cdb <= (int32_t)(DSP_DB_MIN * 100)) {
		shell_fprintf(sh, SHELL_NORMAL, "%s  -inf dBFS", label);
	} else {
		shell_fprintf(sh, SHELL_NORMAL, "%s %s%d.%d dBFS", label, cdb < 0 ? "-" : "",
			      abs(cdb) / 100, abs(cdb) % 100 / 10);
	}
}

static int cmd_audio(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct audio_level_msg lm;
	struct audio_spectrum_msg sm;
	struct audio_event_msg em;

	(void)zbus_chan_read(&audio_level_chan, &lm, K_MSEC(100));
	(void)zbus_chan_read(&audio_spectrum_chan, &sm, K_MSEC(100));
	(void)zbus_chan_read(&audio_event_chan, &em, K_MSEC(100));

	print_cdb(sh, "Level: RMS", lm.rms_cdb);
	print_cdb(sh, ", peak", lm.peak_cdb);
	shell_print(sh, "%s (#%u)", lm.clipped ? ", CLIPPED" : "", lm.seq);

	/* Spectrum: 64 bands as 32 columns (max of each pair), 10 dB per row.
	 * Column c is centred on 40 Hz * 200^((2c + 1) / 64).
	 */
	static const char *const axis = "40   100    300    1k     3k   8k Hz";

	shell_print(sh, "Spectrum #%u (%u-point FFT, %u bands, dB above %d dBFS):", sm.seq,
		    sm.fft_size, sm.n_bands, AUDIO_BAND_DB_MIN);
	for (int row = 9; row >= 0; row--) {
		char line[DSP_BANDS / 2 + 1];

		for (int col = 0; col < DSP_BANDS / 2; col++) {
			int v = MAX(sm.band_db[2 * col], sm.band_db[2 * col + 1]);

			line[col] = v > row * 10 + 5 ? '#' : (v > row * 10 ? '.' : ' ');
		}
		line[DSP_BANDS / 2] = '\0';
		shell_print(sh, "%3d |%s|", row * 10, line);
	}
	shell_print(sh, "     %s", axis);

	uint32_t blocks = (uint32_t)atomic_get(&blocks_seen);
	uint32_t avg_us = blocks ? (uint32_t)(dsp_us_total / blocks) : 0;
	uint32_t block_us = BLOCK_SAMPLES * 1000000U / RATE;

	shell_print(sh, "Claps %u (last event #%u type %u). Analysis per %u us block: "
		    "last %u us, avg %u us, max %u us (%u.%u%% CPU)",
		    (uint32_t)atomic_get(&claps), em.seq, em.type, block_us,
		    (uint32_t)atomic_get(&dsp_us_last), avg_us, (uint32_t)atomic_get(&dsp_us_max),
		    avg_us * 100 / block_us, avg_us * 1000 / block_us % 10);
	/* Read while the capture thread may update them: fine for a status line. */
	shell_print(sh, "Bursts turned down: %u too quiet, %u thumps. Last burst: peak %d dBFS, "
		    "energy around %d Hz (claps need >= %d Hz)",
		    clap.rejected_quiet, clap.rejected_thump,
		    (int)lroundf(clap.last_burst_peak_dbfs), (int)lroundf(clap.last_burst_hz),
		    DSP_CLAP_MIN_HZ);
	if (blocks > 0) {
		shell_fprintf(sh, SHELL_NORMAL, "  avg per stage:");
		for (int i = 0; i < STAGE_COUNT; i++) {
			shell_fprintf(sh, SHELL_NORMAL, " %s %u us", stage_names[i],
				      (uint32_t)(stage_us[i] / blocks));
		}
		shell_print(sh, "");
	}
	return 0;
}

SHELL_CMD_REGISTER(audio, NULL, "Sound level, spectrum, claps and analysis timing", cmd_audio);

static int cmd_mic(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct audio_level_msg lm;

	(void)zbus_chan_read(&audio_level_chan, &lm, K_MSEC(100));
	shell_print(sh, "%s: %s, %u Hz mono (%s mic), %u-sample blocks, %u blocks analyzed",
		    dmic->name, atomic_get(&running) ? "running" : "stopped", RATE,
		    atomic_get(&cur_right) == 1 ? "right" : "left", BLOCK_SAMPLES,
		    (uint32_t)atomic_get(&blocks_seen));
	print_cdb(sh, "Level: RMS", lm.rms_cdb);
	print_cdb(sh, ", peak", lm.peak_cdb);
	shell_print(sh, "");
	return 0;
}

static int cmd_mic_start(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "left") == 0) {
			atomic_set(&want_right, 0);
		} else if (strcmp(argv[1], "right") == 0) {
			atomic_set(&want_right, 1);
		} else {
			shell_error(sh, "Usage: mic start [left|right]");
			return -EINVAL;
		}
	}

	atomic_set(&want_running, 1);
	k_sem_give(&run_sem);
	return 0;
}

static int cmd_mic_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	atomic_set(&want_running, 0);
	return 0;
}

/* "mic level [seconds]": one line per 100 ms with a bar from -90 to 0 dBFS. */
static int cmd_mic_level(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long secs = argc > 1 ? shell_strtoul(argv[1], 10, &err) : 3;

	if (err != 0 || secs < 1 || secs > 60) {
		shell_error(sh, "Usage: mic level [1..60 seconds]");
		return -EINVAL;
	}

	for (unsigned long i = 0; i < secs * 10; i++) {
		struct audio_level_msg lm;

		(void)zbus_chan_read(&audio_level_chan, &lm, K_MSEC(100));

		int bar = CLAMP((lm.rms_cdb + 9000) / 200, 0, 45); /* 2 dB per character */
		char line[46];

		memset(line, '#', bar);
		line[bar] = '\0';
		print_cdb(sh, "RMS", lm.rms_cdb);
		print_cdb(sh, "  peak", lm.peak_cdb);
		shell_print(sh, "  |%-45s|", line);
		k_msleep(100);
	}
	return 0;
}

static int cmd_mic_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct dmic_psoc6_pdm_stats st;

	dmic_psoc6_pdm_get_stats(dmic, &st);
	shell_print(sh, "Mode %s, PDM clock %u Hz, nominal rate %u Hz", st.dma ? "DMA" : "FIFO IRQ",
		    st.pdm_clk_hz, st.rate_hz);
	shell_print(sh, "Blocks %u, dropped %u, FIFO overflows %u, IRQs %u, DMA errors %u",
		    st.blocks, st.dropped, st.fifo_overflows, st.irqs, st.dma_errors);

	int64_t ticks = st.last_block_ticks - st.first_block_ticks;

	if (ticks > 0) {
		/* Frames per second over the measurement window, in milli-Hz. */
		uint64_t mhz = st.frames * 1000ULL * CONFIG_SYS_CLOCK_TICKS_PER_SEC / ticks;

		shell_print(sh, "Measured rate %llu.%03llu Hz over %lld ms", mhz / 1000, mhz % 1000,
			    k_ticks_to_ms_floor64(ticks));
	}
	return 0;
}

/* "mic dump [n]": the next n samples, 16 per line, for offline analysis. */
static int cmd_mic_dump(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	unsigned long n = argc > 1 ? shell_strtoul(argv[1], 10, &err) : 1024;

	if (err != 0 || n < 1 || n > DUMP_MAX) {
		shell_error(sh, "Usage: mic dump [1..%d samples]", DUMP_MAX);
		return -EINVAL;
	}
	if (!atomic_get(&running)) {
		shell_error(sh, "Microphone not running");
		return -EIO;
	}

	k_sem_reset(&dump_done);
	dump_have = 0;
	atomic_set(&dump_want, n);
	if (k_sem_take(&dump_done, K_MSEC(n * 1000 / RATE + 1000)) != 0) {
		atomic_set(&dump_want, 0);
		shell_error(sh, "Timed out");
		return -ETIMEDOUT;
	}

	shell_print(sh, "# %lu samples, %u Hz", n, RATE);
	for (size_t i = 0; i < n; i += 16) {
		char line[16 * 8];
		int len = 0;

		for (size_t j = i; j < MIN(i + 16, n); j++) {
			len += snprintk(line + len, sizeof(line) - len, "%d ", dump_buf[j]);
		}
		shell_print(sh, "%s", line);
	}
	shell_print(sh, "# end");
	return 0;
}

static int cmd_mic_rec(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!atomic_get(&running)) {
		shell_error(sh, "Microphone not running");
		return -EIO;
	}

	int err = audio_rec_start();

	if (err == -ENOTSUP) {
		shell_error(sh, "No recorder in this build (build with rec.conf)");
	} else if (err == -EBUSY) {
		shell_error(sh, "Already recording");
	} else {
		shell_print(sh, "Recording %d s with the LED on, then GET /api/rec.wav",
			    CONFIG_APP_AUDIO_REC_SECONDS);
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(mic_cmds,
	SHELL_CMD_ARG(start, NULL, "Start capturing: mic start [left|right]", cmd_mic_start, 1, 1),
	SHELL_CMD(stop, NULL, "Stop capturing", cmd_mic_stop),
	SHELL_CMD_ARG(level, NULL, "Live level meter: mic level [seconds]", cmd_mic_level, 1, 1),
	SHELL_CMD(stats, NULL, "Driver counters and measured sample rate", cmd_mic_stats),
	SHELL_CMD_ARG(dump, NULL, "Print raw samples: mic dump [n]", cmd_mic_dump, 1, 1),
	SHELL_CMD(rec, NULL, "Record raw audio for /api/rec.wav (rec.conf builds)", cmd_mic_rec),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(mic, &mic_cmds, "Microphone status", cmd_mic);
