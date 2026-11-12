/*
 * zbus channels shared between modules, and their message types.
 *
 * Every module is a publisher or an observer: nothing calls another module
 * directly. A channel always holds its latest message, so any module (the
 * shell, a newly connected web client) can read the current state at any time.
 *
 *   temp_chan            thermistor  -> web, datalog, shell
 *   audio_level_chan     audio DSP   -> web, datalog
 *   audio_spectrum_chan  audio DSP   -> web
 *   audio_event_chan     audio DSP   -> ui_ctrl, web, datalog
 *   input_chan           CapSense    -> web (live touch view)
 *   ctrl_chan            CapSense, web, shell -> ui_ctrl (commands)
 *   ui_chan              ui_ctrl     -> web, audio DSP, datalog
 *   net_chan             Wi-Fi mgr   -> web, datalog, shell
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CHANNELS_H_
#define CHANNELS_H_

#include <stdint.h>
#include <stdlib.h>
#include <zephyr/zbus/zbus.h>

/* ---- Temperature ------------------------------------------------------------ */

/* Published once per thermistor sample (1 Hz). */
struct temp_msg {
	uint32_t seq;         /* sample number, 0 = no reading yet */
	int32_t centi_c;      /* temperature in hundredths of a degree C */
	uint32_t ntc_ohm;
	uint16_t raw_a;
	uint16_t raw_b;
	int32_t latency_us;   /* counter IRQ -> sample start, -1 if not timer-triggered */
	int64_t timestamp_ms; /* k_uptime_get() when sampled */
};

ZBUS_CHAN_DECLARE(temp_chan);

/* ---- Audio ------------------------------------------------------------------ */

/* Sound level over one analysis window, in hundredths of a dB relative to
 * digital full scale (-4512 = -45.12 dBFS).
 */
struct audio_level_msg {
	uint32_t seq;
	int16_t rms_cdb;
	int16_t peak_cdb;
	uint8_t clipped;      /* 1 if a sample reached full scale in this window */
	int64_t timestamp_ms;
};

ZBUS_CHAN_DECLARE(audio_level_chan);

#define AUDIO_BANDS       64
#define AUDIO_BAND_DB_MIN (-100) /* band value 0 */

/* FFT magnitude spectrum, reduced to AUDIO_BANDS log-spaced bands. */
struct audio_spectrum_msg {
	uint32_t seq;
	uint16_t sample_rate_hz;
	uint16_t fft_size;
	uint8_t n_bands;
	uint8_t band_db[AUDIO_BANDS]; /* dB above AUDIO_BAND_DB_MIN, 0..100 */
	int64_t timestamp_ms;
};

ZBUS_CHAN_DECLARE(audio_spectrum_chan);

enum audio_event_type {
	AUDIO_EVT_CLAP = 1,
	AUDIO_EVT_DOUBLE_CLAP,
	AUDIO_EVT_CLIP,
};

struct audio_event_msg {
	uint32_t seq;
	uint8_t type;         /* enum audio_event_type */
	int16_t peak_cdb;     /* peak level of the event */
	int64_t timestamp_ms;
};

ZBUS_CHAN_DECLARE(audio_event_chan);

/* ---- Inputs and commands ---------------------------------------------------- */

#define INPUT_SLIDER_MAX 300 /* slider resolution from the CapSense configuration */

/* Live CapSense state, published whenever it changes. */
struct input_msg {
	uint32_t seq;
	uint8_t btn0;
	uint8_t btn1;
	int16_t slider;       /* 0..INPUT_SLIDER_MAX, -1 = not touched */
};

ZBUS_CHAN_DECLARE(input_chan);

enum ctrl_source {
	CTRL_SRC_BOOT,
	CTRL_SRC_CAPSENSE,
	CTRL_SRC_WEB,
	CTRL_SRC_SHELL,
};

enum ctrl_cmd {
	CTRL_VIEW_NEXT,       /* value unused */
	CTRL_VIEW_SET,        /* value: enum ui_view */
	CTRL_COLOUR_NEXT,     /* value unused */
	CTRL_COLOUR_SET,      /* value: colour scheme index */
	CTRL_PARAM_SET,       /* param: enum ui_param, value: 0..100 */
	CTRL_SLIDER_PARAM_SET,/* value: enum ui_param the slider adjusts */
	CTRL_FREEZE_TOGGLE,   /* value unused */
	CTRL_FREEZE_SET,      /* value: 0/1 */
	CTRL_LED_CLAP_SET,    /* value: 0/1, flash the user LED on claps */
	CTRL_CMD_COUNT,
};

/* A request to change the UI state. Commands only come through here, whoever
 * sends them, so ui_ctrl is the single place that validates and applies them.
 */
struct ctrl_msg {
	uint8_t source;       /* enum ctrl_source */
	uint8_t cmd;          /* enum ctrl_cmd */
	uint8_t param;        /* enum ui_param, for CTRL_PARAM_SET */
	int32_t value;
};

ZBUS_CHAN_DECLARE(ctrl_chan);

/* ---- UI state --------------------------------------------------------------- */

enum ui_view {
	UI_VIEW_BARS,         /* spectrum as bars */
	UI_VIEW_LINE,         /* spectrum as a line with peak hold */
	UI_VIEW_WATERFALL,    /* scrolling spectrogram */
	UI_VIEW_LEVEL,        /* sound level meter only */
	UI_VIEW_COUNT,
};

#define UI_COLOUR_COUNT 4

enum ui_param {
	UI_PARAM_GAIN,        /* display gain, 0..100 (50 = 0 dB) */
	UI_PARAM_SMOOTHING,   /* spectrum smoothing, 0..100 */
	UI_PARAM_CLAP_SENS,   /* clap detector sensitivity, 0..100 */
	UI_PARAM_COUNT,
};

#define UI_PARAM_MAX 100

/* Dashboard state, owned by ui_ctrl. Everything that renders (web clients,
 * the LED) follows this channel, so all of them always agree.
 */
struct ui_msg {
	uint32_t seq;
	uint8_t view;         /* enum ui_view */
	uint8_t colour;       /* 0..UI_COLOUR_COUNT-1 */
	uint8_t freeze;       /* 1 = display frozen */
	uint8_t led_on_clap;  /* 1 = flash the user LED on claps */
	uint8_t slider_param; /* enum ui_param the CapSense slider adjusts */
	uint8_t source;       /* enum ctrl_source of the last change */
	uint8_t param[UI_PARAM_COUNT];
};

ZBUS_CHAN_DECLARE(ui_chan);

/* ---- Network ---------------------------------------------------------------- */

enum net_state {
	NET_STATE_IDLE,       /* no credentials stored */
	NET_STATE_CONNECTING,
	NET_STATE_CONNECTED,  /* associated, waiting for an address */
	NET_STATE_ONLINE,     /* has an IPv4 address */
};

struct net_msg {
	uint8_t state;        /* enum net_state */
	int8_t rssi;          /* dBm, 0 = unknown */
	char ssid[33];
	char ipv4[16];
};

ZBUS_CHAN_DECLARE(net_chan);

/* ---- Helpers ---------------------------------------------------------------- */

/* printf helpers for hundredths: printk(CENTI_FMT " C", CENTI_ARGS(v)) */
#define CENTI_FMT       "%s%d.%02d"
#define CENTI_ARGS(v)   ((v) < 0 ? "-" : ""), abs(v) / 100, abs(v) % 100

#endif /* CHANNELS_H_ */
