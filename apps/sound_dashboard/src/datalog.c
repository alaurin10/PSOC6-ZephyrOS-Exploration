/*
 * Data log on the QSPI flash.
 *
 * CSV, with a header in each file:
 *
 *   time,uptime_s,kind,temp_c,rms_dbfs,peak_dbfs,claps,detail
 *
 * - kind "sample", every second: the temperature, the sound level over that
 *   second (RMS, power-averaged, and peak) and how many claps it held;
 * - events: "clap", "double" and "clip" (with their peak level), "ui" (the
 *   dashboard state after a change, at most once a second), "wifi"
 *   (connection changes), "boot" (with the reset cause), "clock" (time set
 *   from SNTP) and "note" (from the shell).
 * "time" is UTC (ISO 8601) once SNTP has set the clock and empty before;
 * "uptime_s" is always there.
 *
 * zbus listeners and a 1 s timer only collect: they put records in a queue
 * and never wait for the flash. A low-priority writer thread turns them into
 * lines in a RAM buffer, and every CONFIG_APP_DATALOG_SYNC_S seconds writes
 * the buffer to the file and syncs it, so a power cut loses at most that
 * much. Files are log00001.csv, log00002.csv, ...: a new one starts after
 * CONFIG_APP_DATALOG_FILE_KB, and the oldest go to keep
 * CONFIG_APP_DATALOG_FILES.
 *
 * The file system is LittleFS on the log partition of the QSPI flash, whose
 * erase blocks are 256 KB. This module mounts it itself, to cap the size of
 * LittleFS's metadata logs (metadata_max): they'd otherwise grow to a whole
 * block, and LittleFS rescans them on every open and sync.
 *
 * A sector erase can take up to 3 s, longer than a watchdog check-in period.
 * So the 1 s timer checks in for the writer while the writer is waiting for
 * records or inside a file operation shorter than IO_STALL_MS.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "app_time.h"
#include "app_watchdog.h"
#include "channels.h"
#include "datalog.h"
#include "ui_ctrl.h"

LOG_MODULE_REGISTER(datalog, LOG_LEVEL_INF);

#define WRITER_PRIORITY   12
#define WRITER_STACK_SIZE 3072
#define QUEUE_LEN         48
#define TICK_MS           1000
#define SYNC_MS           (CONFIG_APP_DATALOG_SYNC_S * 1000)
#define FILE_MAX_BYTES    ((uint32_t)CONFIG_APP_DATALOG_FILE_KB * 1024U)
#define BUF_SIZE          2048
#define LINE_MAX          224
#define METADATA_MAX      (32 * 1024)
#define IO_STALL_MS       15000 /* far beyond any healthy erase or compaction */
#define PATH_LEN          (sizeof(DATALOG_MOUNT_POINT) + DATALOG_NAME_LEN)
#define HEADER            "time,uptime_s,kind,temp_c,rms_dbfs,peak_dbfs,claps,detail\n"

#define LOG_PARTITION DT_NODELABEL(log_partition)

/* Leave LittleFS room for its metadata and copy-on-write blocks. */
BUILD_ASSERT((uint64_t)CONFIG_APP_DATALOG_FILES * (CONFIG_APP_DATALOG_FILE_KB + 256) * 1024 <=
		     DT_REG_SIZE(LOG_PARTITION) * 3 / 4,
	     "CONFIG_APP_DATALOG_FILES x CONFIG_APP_DATALOG_FILE_KB is too big for the partition");

/* ---- Records ------------------------------------------------------------------------ */

enum rec_kind {
	REC_SAMPLE,
	REC_AUDIO,
	REC_UI,
	REC_NET,
	REC_NOTE,
	REC_FLUSH, /* datalog_flush() */
	REC_CLEAR, /* datalog_clear() */
};

struct rec {
	uint8_t kind;
	int64_t uptime_ms;
	union {
		struct {
			float power_sum;  /* linear RMS power, summed over the level messages */
			uint16_t levels;  /* level messages in this second */
			int16_t peak_cdb;
			uint16_t claps;
			bool temp_valid;
			int32_t temp_cc;
		} s;
		struct audio_event_msg ev;
		struct ui_msg ui;
		struct net_msg net;
		char note[48];
	};
};

K_MSGQ_DEFINE(log_q, sizeof(struct rec), QUEUE_LEN, 8);

static atomic_t running; /* the writer has a file open: start collecting */
static atomic_t dropped;

static void put(const struct rec *r)
{
	if (k_msgq_put(&log_q, r, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
	}
}

/* ---- Collecting: zbus listeners and the 1 s tick ----------------------------------- */

/* What the current second has seen so far. */
static struct k_spinlock acc_lock;
static struct {
	float power_sum;
	uint16_t levels;
	int16_t peak_cdb;
	uint16_t claps;
	bool temp_valid;
	int32_t temp_cc;
	bool ui_dirty;
	struct ui_msg ui;
} acc = {.peak_cdb = INT16_MIN};

static struct net_msg last_net; /* only touched by the listener (net_chan's one publisher) */

static void datalog_listener(const struct zbus_channel *chan)
{
	if (!atomic_get(&running)) {
		return;
	}

	if (chan == &audio_level_chan) {
		const struct audio_level_msg *m = zbus_chan_const_msg(chan);
		float power = powf(10.0f, (float)m->rms_cdb / 1000.0f);

		K_SPINLOCK(&acc_lock) {
			acc.power_sum += power;
			acc.levels++;
			acc.peak_cdb = MAX(acc.peak_cdb, m->peak_cdb);
		}
	} else if (chan == &temp_chan) {
		const struct temp_msg *m = zbus_chan_const_msg(chan);

		K_SPINLOCK(&acc_lock) {
			acc.temp_cc = m->centi_c;
			acc.temp_valid = m->seq != 0;
		}
	} else if (chan == &audio_event_chan) {
		const struct audio_event_msg *m = zbus_chan_const_msg(chan);
		struct rec r = {.kind = REC_AUDIO, .uptime_ms = m->timestamp_ms, .ev = *m};

		if (m->type == AUDIO_EVT_CLAP || m->type == AUDIO_EVT_DOUBLE_CLAP) {
			K_SPINLOCK(&acc_lock) {
				acc.claps++;
			}
		}
		put(&r);
	} else if (chan == &ui_chan) {
		/* Slider drags change it many times a second: keep only the latest. */
		K_SPINLOCK(&acc_lock) {
			acc.ui = *(const struct ui_msg *)zbus_chan_const_msg(chan);
			acc.ui_dirty = true;
		}
	} else if (chan == &net_chan) {
		const struct net_msg *m = zbus_chan_const_msg(chan);

		/* Log changes of state, network or address, not every signal reading. */
		if (m->state != last_net.state || strcmp(m->ssid, last_net.ssid) != 0 ||
		    strcmp(m->ipv4, last_net.ipv4) != 0) {
			struct rec r = {.kind = REC_NET, .uptime_ms = k_uptime_get(), .net = *m};

			last_net = *m;
			put(&r);
		}
	}
}

ZBUS_LISTENER_DEFINE(datalog_lis, datalog_listener);
ZBUS_CHAN_ADD_OBS(audio_level_chan, datalog_lis, 6);
ZBUS_CHAN_ADD_OBS(temp_chan, datalog_lis, 6);
ZBUS_CHAN_ADD_OBS(audio_event_chan, datalog_lis, 6);
ZBUS_CHAN_ADD_OBS(ui_chan, datalog_lis, 6);
ZBUS_CHAN_ADD_OBS(net_chan, datalog_lis, 6);

/* Watchdog: the writer counts as alive while waiting for records, or while in
 * a file operation that hasn't taken too long.
 */
static atomic_t io_since; /* k_uptime_get_32() | 1 at the start of a file operation, 0 = none */
static atomic_t loop_at;  /* k_uptime_get_32() at the writer's last loop */

static void io_begin(void)
{
	atomic_set(&io_since, (atomic_val_t)(k_uptime_get_32() | 1U));
}

static void io_end(void)
{
	atomic_set(&io_since, 0);
}

static void check_in_for_writer(void)
{
	uint32_t now = k_uptime_get_32();
	uint32_t io = (uint32_t)atomic_get(&io_since);
	bool alive = io != 0 ? now - io < IO_STALL_MS
			     : now - (uint32_t)atomic_get(&loop_at) < 2 * TICK_MS + 500;

	if (alive) {
		app_watchdog_alive(WDT_CLIENT_LOG);
	}
}

/* Runs in interrupt context every second: snapshot the second, queue it. */
static void tick(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	struct rec r = {.kind = REC_SAMPLE, .uptime_ms = k_uptime_get()};
	struct rec u = {.kind = REC_UI, .uptime_ms = r.uptime_ms};
	bool ui_dirty;

	check_in_for_writer();
	if (!atomic_get(&running)) {
		return; /* still mounting */
	}

	K_SPINLOCK(&acc_lock) {
		r.s.power_sum = acc.power_sum;
		r.s.levels = acc.levels;
		r.s.peak_cdb = acc.peak_cdb;
		r.s.claps = acc.claps;
		r.s.temp_valid = acc.temp_valid;
		r.s.temp_cc = acc.temp_cc;
		acc.power_sum = 0.0f;
		acc.levels = 0;
		acc.peak_cdb = INT16_MIN;
		acc.claps = 0;
		ui_dirty = acc.ui_dirty;
		u.ui = acc.ui;
		acc.ui_dirty = false;
	}
	put(&r);
	if (ui_dirty) {
		put(&u);
	}
}

K_TIMER_DEFINE(tick_timer, tick, NULL);

/* ---- File names ---------------------------------------------------------------------- */

static bool parse_name(const char *name, uint32_t *seq)
{
	if (strlen(name) != 12 || strncmp(name, "log", 3) != 0 || strcmp(name + 8, ".csv") != 0) {
		return false;
	}
	for (int i = 3; i < 8; i++) {
		if (!isdigit((unsigned char)name[i])) {
			return false;
		}
	}
	*seq = strtoul(name + 3, NULL, 10);
	return *seq > 0;
}

static void name_of(uint32_t seq, char *name)
{
	snprintk(name, DATALOG_NAME_LEN, "log%05u.csv", seq % 100000U);
}

int datalog_path(const char *name, char *path, size_t len)
{
	uint32_t seq;

	if (name == NULL || !parse_name(name, &seq)) {
		return -EINVAL;
	}
	snprintk(path, len, DATALOG_MOUNT_POINT "/%s", name);
	return 0;
}

/* Oldest and newest log file, and how many there are. */
struct scan_result {
	uint32_t first;
	uint32_t last;
	uint32_t last_size;
	int count;
};

static int scan(struct scan_result *res, struct datalog_file *files, int max)
{
	struct fs_dir_t dir;
	struct fs_dirent e;
	int rc;

	*res = (struct scan_result){0};
	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, DATALOG_MOUNT_POINT);
	if (rc < 0) {
		return rc;
	}
	while (fs_readdir(&dir, &e) == 0 && e.name[0] != '\0') {
		uint32_t seq;

		if (e.type != FS_DIR_ENTRY_FILE || !parse_name(e.name, &seq)) {
			continue;
		}
		if (res->count == 0 || seq < res->first) {
			res->first = seq;
		}
		if (res->count == 0 || seq > res->last) {
			res->last = seq;
			res->last_size = e.size;
		}
		if (files != NULL && res->count < max) {
			/* Insertion sort by sequence number: names have fixed width. */
			int i = res->count;

			while (i > 0 && strcmp(files[i - 1].name, e.name) > 0) {
				files[i] = files[i - 1];
				i--;
			}
			strncpy(files[i].name, e.name, DATALOG_NAME_LEN - 1);
			files[i].name[DATALOG_NAME_LEN - 1] = '\0';
			files[i].size = e.size;
		}
		res->count++;
	}
	fs_closedir(&dir);
	return 0;
}

/* ---- The writer ---------------------------------------------------------------------- */

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);

static struct fs_mount_t mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_data,
	.storage_dev = (void *)PARTITION_ID(log_partition),
	.mnt_point = DATALOG_MOUNT_POINT,
};

static struct fs_file_t file;
static bool file_open;
static uint32_t cur_seq;
static uint32_t cur_size;
static char buf[BUF_SIZE];
static size_t buf_len;

static struct k_spinlock stats_lock;
static struct datalog_stats stats = {.last_sync_ms = -1};

static K_SEM_DEFINE(req_done, 0, 1);
static K_MUTEX_DEFINE(req_lock);
static int req_result;

static void count_error(void)
{
	K_SPINLOCK(&stats_lock) {
		stats.errors++;
	}
}

static int open_current(uint32_t seq)
{
	char name[DATALOG_NAME_LEN];
	char path[PATH_LEN];
	struct fs_dirent st;
	int rc;

	name_of(seq, name);
	snprintk(path, sizeof(path), DATALOG_MOUNT_POINT "/%s", name);
	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc < 0) {
		LOG_ERR("Can't open %s (%d)", path, rc);
		return rc;
	}
	file_open = true;
	cur_seq = seq;
	cur_size = fs_stat(path, &st) == 0 ? st.size : 0;
	if (cur_size == 0) {
		memcpy(buf + buf_len, HEADER, sizeof(HEADER) - 1);
		buf_len += sizeof(HEADER) - 1;
	}
	K_SPINLOCK(&stats_lock) {
		strcpy(stats.current, name);
	}
	return 0;
}

/* Delete the oldest files until there's room for one more. */
static void prune(void)
{
	struct scan_result res;

	while (scan(&res, NULL, 0) == 0 && res.count >= CONFIG_APP_DATALOG_FILES) {
		char name[DATALOG_NAME_LEN];
		char path[PATH_LEN];

		name_of(res.first, name);
		snprintk(path, sizeof(path), DATALOG_MOUNT_POINT "/%s", name);
		if (fs_unlink(path) < 0) {
			count_error();
			break;
		}
		LOG_INF("Deleted %s (oldest)", name);
	}
}

static int start_new_file(uint32_t seq)
{
	if (file_open) {
		fs_close(&file);
		file_open = false;
	}
	prune();
	return open_current(seq);
}

/* Write the buffered lines to the file, and commit them if sync. */
static int write_out(bool sync)
{
	int64_t t0 = k_uptime_get();
	int rc = 0;

	if (!file_open) {
		buf_len = 0;
		return -EBADF;
	}

	io_begin();
	if (buf_len > 0) {
		ssize_t n = fs_write(&file, buf, buf_len);

		if (n == (ssize_t)buf_len) {
			cur_size += n;
			K_SPINLOCK(&stats_lock) {
				stats.bytes += n;
			}
		} else {
			rc = n < 0 ? (int)n : -EIO;
		}
		buf_len = 0; /* on failure, drop them rather than retry forever */
	}
	if (rc == 0 && sync) {
		rc = fs_sync(&file);
	}
	io_end();

	uint32_t took = (uint32_t)(k_uptime_get() - t0);

	K_SPINLOCK(&stats_lock) {
		stats.max_write_ms = MAX(stats.max_write_ms, took);
		if (rc == 0 && sync) {
			stats.last_sync_ms = k_uptime_get();
		}
	}
	if (rc < 0) {
		LOG_ERR("Writing the log failed (%d)", rc);
		count_error();
	}
	if (cur_size >= FILE_MAX_BYTES) {
		io_begin();
		(void)start_new_file(cur_seq + 1);
		io_end();
	}
	return rc;
}

/* ---- Formatting ------------------------------------------------------------------------ */

/* Hundredths of a dB with one decimal: -5823 -> "-58.2". */
static void fmt_cdb(char *out, size_t len, int cdb)
{
	int tenths = (abs(cdb) + 5) / 10;

	snprintk(out, len, "%s%d.%d", cdb < 0 ? "-" : "", tenths / 10, tenths % 10);
}

/* A CSV field in quotes, with quotes inside doubled. */
static int csv_quoted(char *out, size_t len, const char *s)
{
	size_t n = 0;

	if (len < 3) {
		return 0;
	}
	out[n++] = '"';
	for (; *s != '\0' && n < len - 3; s++) {
		if (*s == '"') {
			if (n >= len - 4) {
				break;
			}
			out[n++] = '"';
		}
		out[n++] = (*s == '\n' || *s == '\r') ? ' ' : *s;
	}
	out[n++] = '"';
	out[n] = '\0';
	return (int)n;
}

/* "time,uptime_s,kind," */
static int fmt_prefix(char *out, size_t len, int64_t uptime_ms, const char *kind)
{
	char t[32] = "";

	if (app_time_valid()) {
		app_time_format(uptime_ms, t, sizeof(t));
	}
	return snprintk(out, len, "%s,%lld.%03d,%s,", t, uptime_ms / 1000,
			(int)(uptime_ms % 1000), kind);
}

/* An event: empty measurement columns, then the quoted detail. */
static int fmt_event(char *out, size_t len, int64_t uptime_ms, const char *kind,
		     const char *detail)
{
	int n = fmt_prefix(out, len, uptime_ms, kind);

	n += snprintk(out + n, len - n, ",,,,");
	n += csv_quoted(out + n, len - n - 1, detail);
	n += snprintk(out + n, len - n, "\n");
	return n;
}

static int fmt_record(const struct rec *r, char *out, size_t len)
{
	char detail[LINE_MAX / 2];
	char a[16] = "";
	char b[16] = "";
	char c[16] = "";
	int n;

	switch (r->kind) {
	case REC_SAMPLE:
		if (r->s.temp_valid) {
			snprintk(a, sizeof(a), CENTI_FMT, CENTI_ARGS(r->s.temp_cc));
		}
		if (r->s.levels > 0) {
			fmt_cdb(b, sizeof(b),
				(int)lroundf(1000.0f * log10f(r->s.power_sum / r->s.levels)));
			fmt_cdb(c, sizeof(c), r->s.peak_cdb);
		}
		n = fmt_prefix(out, len, r->uptime_ms, "sample");
		return n + snprintk(out + n, len - n, "%s,%s,%s,%u,\n", a, b, c, r->s.claps);

	case REC_AUDIO:
		n = fmt_prefix(out, len, r->uptime_ms,
			       audio_event_names[r->ev.type % AUDIO_EVT_COUNT]);
		if (r->ev.type != AUDIO_EVT_CLIP) {
			fmt_cdb(c, sizeof(c), r->ev.peak_cdb);
		}
		return n + snprintk(out + n, len - n, ",,%s,,\n", c);

	case REC_UI:
		snprintk(detail, sizeof(detail),
			 "view=%s colour=%u freeze=%u ledclap=%u slider=%s %s=%u %s=%u %s=%u "
			 "src=%s",
			 ui_view_names[r->ui.view % UI_VIEW_COUNT], r->ui.colour, r->ui.freeze,
			 r->ui.led_on_clap, ui_param_names[r->ui.slider_param % UI_PARAM_COUNT],
			 ui_param_names[0], r->ui.param[0], ui_param_names[1], r->ui.param[1],
			 ui_param_names[2], r->ui.param[2],
			 ctrl_source_names[r->ui.source % CTRL_SRC_COUNT]);
		return fmt_event(out, len, r->uptime_ms, "ui", detail);

	case REC_NET:
		snprintk(detail, sizeof(detail), "%s %s rssi=%d ssid=%s",
			 net_state_names[r->net.state % NET_STATE_COUNT],
			 r->net.ipv4[0] ? r->net.ipv4 : "-", r->net.rssi, r->net.ssid);
		return fmt_event(out, len, r->uptime_ms, "wifi", detail);

	case REC_NOTE:
		return fmt_event(out, len, r->uptime_ms, "note", r->note);

	default:
		return 0;
	}
}

static void append_line(const char *line, int n)
{
	if (n <= 0) {
		return;
	}
	if (buf_len + n > sizeof(buf)) {
		(void)write_out(false);
	}
	memcpy(buf + buf_len, line, MIN((size_t)n, sizeof(buf)));
	buf_len += MIN((size_t)n, sizeof(buf));
	K_SPINLOCK(&stats_lock) {
		stats.lines++;
	}
}

static void log_event(int64_t uptime_ms, const char *kind, const char *detail)
{
	char line[LINE_MAX];

	append_line(line, fmt_event(line, sizeof(line), uptime_ms, kind, detail));
}

/* ---- Requests from other threads ---------------------------------------------------- */

static int delete_all(void)
{
	struct datalog_file files[CONFIG_APP_DATALOG_FILES + 4];
	struct scan_result res;
	int rc = scan(&res, files, ARRAY_SIZE(files));

	for (int i = 0; rc == 0 && i < MIN(res.count, (int)ARRAY_SIZE(files)); i++) {
		char path[PATH_LEN];

		snprintk(path, sizeof(path), DATALOG_MOUNT_POINT "/%.15s", files[i].name);
		rc = fs_unlink(path);
	}
	return rc;
}

static void handle_request(const struct rec *r)
{
	int rc;

	if (r->kind == REC_FLUSH) {
		rc = write_out(true);
	} else {
		buf_len = 0;
		if (file_open) {
			fs_close(&file);
			file_open = false;
		}
		io_begin();
		rc = delete_all();
		if (rc == 0) {
			rc = open_current(1);
		}
		io_end();
		if (rc == 0) {
			log_event(k_uptime_get(), "note", "log cleared");
			rc = write_out(true);
		}
	}
	req_result = rc;
	k_sem_give(&req_done);
}

static int request(uint8_t kind, int timeout_ms)
{
	struct rec r = {.kind = kind, .uptime_ms = k_uptime_get()};
	int rc;

	if (!atomic_get(&running)) {
		return -ENODEV;
	}
	k_mutex_lock(&req_lock, K_FOREVER);
	k_sem_reset(&req_done);
	rc = k_msgq_put(&log_q, &r, K_MSEC(100));
	if (rc == 0) {
		rc = k_sem_take(&req_done, K_MSEC(timeout_ms)) == 0 ? req_result : -ETIMEDOUT;
	}
	k_mutex_unlock(&req_lock);
	return rc;
}

int datalog_flush(int timeout_ms)
{
	return request(REC_FLUSH, timeout_ms);
}

int datalog_clear(int timeout_ms)
{
	return request(REC_CLEAR, timeout_ms);
}

void datalog_note(const char *text)
{
	struct rec r = {.kind = REC_NOTE, .uptime_ms = k_uptime_get()};

	strncpy(r.note, text, sizeof(r.note) - 1);
	put(&r);
}

/* ---- Writer thread ------------------------------------------------------------------- */

static int mount_and_open(void)
{
	struct scan_result res;
	int64_t t0 = k_uptime_get();
	int rc;

	lfs_data.cfg.metadata_max = METADATA_MAX;
	rc = fs_mount(&mnt); /* formats the partition if it holds no file system */
	if (rc < 0) {
		LOG_ERR("Mounting %s failed (%d)", DATALOG_MOUNT_POINT, rc);
		return rc;
	}
	LOG_INF("%s mounted in %lld ms", DATALOG_MOUNT_POINT, k_uptime_get() - t0);

	rc = scan(&res, NULL, 0);
	if (rc < 0) {
		return rc;
	}
	if (res.count > 0 && res.last_size < FILE_MAX_BYTES) {
		rc = open_current(res.last); /* carry on in the newest file */
	} else {
		rc = start_new_file(res.count > 0 ? res.last + 1 : 1);
	}
	return rc;
}

static void writer_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	char line[LINE_MAX];
	bool clock_logged = false;
	int64_t next_sync;
	int rc;

	atomic_set(&loop_at, (atomic_val_t)k_uptime_get_32());
	app_watchdog_enable(WDT_CLIENT_LOG);
	/* Until the tick timer runs, nothing else checks in for this thread. */
	k_timer_start(&tick_timer, K_MSEC(TICK_MS), K_MSEC(TICK_MS));

	io_begin();
	rc = mount_and_open();
	io_end();
	if (rc < 0) {
		LOG_ERR("No data log (%d)", rc);
		k_timer_stop(&tick_timer);
		app_watchdog_disable(WDT_CLIENT_LOG);
		return;
	}

	K_SPINLOCK(&stats_lock) {
		stats.mounted = true;
	}
	app_watchdog_reset_cause_str(line, sizeof(line));
	log_event(k_uptime_get(), "boot", line);
	LOG_INF("Logging to %s/%s", DATALOG_MOUNT_POINT, stats.current);

	/* Start from the current dashboard and network state. */
	struct rec r = {.kind = REC_UI, .uptime_ms = k_uptime_get()};

	(void)zbus_chan_read(&ui_chan, &r.ui, K_MSEC(100));
	append_line(line, fmt_record(&r, line, sizeof(line)));
	atomic_set(&running, 1);
	next_sync = k_uptime_get() + SYNC_MS;

	while (true) {
		atomic_set(&loop_at, (atomic_val_t)k_uptime_get_32());

		int64_t wait = CLAMP(next_sync - k_uptime_get(), 0, TICK_MS);

		if (k_msgq_get(&log_q, &r, K_MSEC(wait)) == 0) {
			if (r.kind == REC_FLUSH || r.kind == REC_CLEAR) {
				handle_request(&r);
				next_sync = k_uptime_get() + SYNC_MS;
				continue;
			}
			append_line(line, fmt_record(&r, line, sizeof(line)));
		}
		if (!clock_logged && app_time_valid()) {
			clock_logged = true;
			log_event(k_uptime_get(), "clock", "set from SNTP");
		}
		if (k_uptime_get() >= next_sync) {
			(void)write_out(true);
			next_sync = k_uptime_get() + SYNC_MS;
		}
	}
}

K_THREAD_DEFINE(datalog_tid, WRITER_STACK_SIZE, writer_thread, NULL, NULL, NULL, WRITER_PRIORITY,
		0, SYS_FOREVER_MS);

void datalog_start(void)
{
	k_thread_name_set(datalog_tid, "datalog");
	k_thread_start(datalog_tid);
}

/* ---- Status, listing, tail ----------------------------------------------------------- */

void datalog_get_stats(struct datalog_stats *out)
{
	struct fs_statvfs vfs;

	K_SPINLOCK(&stats_lock) {
		*out = stats;
	}
	out->dropped = (uint32_t)atomic_get(&dropped);
	if (out->mounted && fs_statvfs(DATALOG_MOUNT_POINT, &vfs) == 0) {
		out->free_bytes = (uint64_t)vfs.f_bfree * vfs.f_frsize;
		out->total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
	}
}

int datalog_list(struct datalog_file *files, int max)
{
	struct scan_result res;
	int rc;

	if (!atomic_get(&running)) {
		return -ENODEV;
	}
	rc = scan(&res, files, max);
	return rc < 0 ? rc : MIN(res.count, max);
}

int datalog_tail(char *out, size_t len, int lines, bool flush)
{
	struct datalog_stats st;
	char path[PATH_LEN];
	struct fs_file_t f;
	off_t size;
	ssize_t n;
	int rc;

	datalog_get_stats(&st);
	if (!st.mounted || datalog_path(st.current, path, sizeof(path)) < 0) {
		return -ENODEV;
	}
	if (flush) {
		(void)datalog_flush(5000);
	}

	fs_file_t_init(&f);
	rc = fs_open(&f, path, FS_O_READ);
	if (rc < 0) {
		return rc;
	}
	size = fs_seek(&f, 0, FS_SEEK_END) == 0 ? fs_tell(&f) : 0;
	(void)fs_seek(&f, MAX(0, size - (off_t)(len - 1)), FS_SEEK_SET);
	n = fs_read(&f, out, len - 1);
	fs_close(&f);
	if (n < 0) {
		return (int)n;
	}
	out[n] = '\0';

	/* Keep the last `lines` complete lines. */
	char *start = out;
	int found = 0;

	for (char *p = out + n - 1; p > out; p--) {
		if (p[-1] == '\n' && ++found == lines) {
			start = p;
			break;
		}
	}
	if (found < lines && size > n) {
		/* The buffer starts mid-line: drop the partial first line. */
		char *nl = strchr(out, '\n');

		start = nl != NULL ? nl + 1 : out + n;
	}
	n = (out + n) - start;
	memmove(out, start, n + 1);
	return (int)n;
}

/* ---- Shell ----------------------------------------------------------------------------- */

static int cmd_datalog(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct datalog_stats st;

	datalog_get_stats(&st);
	if (!st.mounted) {
		shell_print(sh, "Not logging: the log file system isn't mounted");
		return 0;
	}
	shell_print(sh, "%s: %llu KB of %llu KB free; writing %s (%u B buffered)",
		    DATALOG_MOUNT_POINT, st.free_bytes / 1024, st.total_bytes / 1024, st.current,
		    (unsigned int)buf_len);
	shell_print(sh, "Since boot: %u lines, %u B, %u dropped, %u errors; longest write %u ms",
		    st.lines, st.bytes, st.dropped, st.errors, st.max_write_ms);
	if (st.last_sync_ms >= 0) {
		shell_print(sh, "Last sync %lld s ago (every %d s); new file after %d KB, keeping %d",
			    (k_uptime_get() - st.last_sync_ms) / 1000, CONFIG_APP_DATALOG_SYNC_S,
			    CONFIG_APP_DATALOG_FILE_KB, CONFIG_APP_DATALOG_FILES);
	}
	return 0;
}

static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct datalog_file files[CONFIG_APP_DATALOG_FILES + 4];
	int n = datalog_list(files, ARRAY_SIZE(files));

	if (n < 0) {
		shell_error(sh, "Can't list the log (%d)", n);
		return n;
	}
	for (int i = 0; i < n; i++) {
		shell_print(sh, "%s %10u B", files[i].name, files[i].size);
	}
	return 0;
}

static int cmd_tail(const struct shell *sh, size_t argc, char **argv)
{
	static char text[2048];
	int lines = argc > 1 ? atoi(argv[1]) : 10;
	int n = datalog_tail(text, sizeof(text), CLAMP(lines, 1, 40), true);

	if (n < 0) {
		shell_error(sh, "Can't read the log (%d)", n);
		return n;
	}
	shell_fprintf(sh, SHELL_NORMAL, "%s", text);
	return 0;
}

static int cmd_note(const struct shell *sh, size_t argc, char **argv)
{
	char text[48] = "";
	size_t n = 0;

	for (size_t i = 1; i < argc; i++) {
		n += snprintk(text + n, sizeof(text) - n, "%s%s", i > 1 ? " " : "", argv[i]);
		n = MIN(n, sizeof(text) - 1);
	}
	datalog_note(text);
	shell_print(sh, "Noted");
	return 0;
}

static int cmd_sync(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int64_t t0 = k_uptime_get();
	int rc = datalog_flush(10000);

	if (rc < 0) {
		shell_error(sh, "Sync failed (%d)", rc);
	} else {
		shell_print(sh, "Synced in %lld ms", k_uptime_get() - t0);
	}
	return rc;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = datalog_clear(30000);

	if (rc < 0) {
		shell_error(sh, "Clearing the log failed (%d)", rc);
	} else {
		shell_print(sh, "Log cleared");
	}
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(datalog_cmds,
	SHELL_CMD(list, NULL, "Log files and their sizes", cmd_list),
	SHELL_CMD_ARG(tail, NULL, "Last lines of the current file: datalog tail [n]", cmd_tail, 1,
		      1),
	SHELL_CMD_ARG(note, NULL, "Log a note: datalog note <text>", cmd_note, 2, 8),
	SHELL_CMD(sync, NULL, "Write and commit the buffered lines now", cmd_sync),
	SHELL_CMD(clear, NULL, "Delete every log file and start again", cmd_clear),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(datalog, &datalog_cmds, "Data log on the QSPI flash", cmd_datalog);
