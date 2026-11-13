/*
 * Data log on the QSPI flash: CSV files in LittleFS, one line per second
 * (temperature, sound level, claps) plus a line per event.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DATALOG_H_
#define DATALOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DATALOG_MOUNT_POINT "/lfs"
#define DATALOG_NAME_LEN    16 /* "log00042.csv" and a terminator */

struct datalog_file {
	char name[DATALOG_NAME_LEN];
	uint32_t size;
};

struct datalog_stats {
	bool mounted;
	char current[DATALOG_NAME_LEN]; /* file being written, "" if none */
	uint32_t lines;                 /* written since boot */
	uint32_t bytes;
	uint32_t dropped;               /* records lost because the queue was full */
	uint32_t errors;                /* failed file operations */
	int64_t last_sync_ms;           /* uptime of the last sync, -1 = none yet */
	uint32_t max_write_ms;          /* longest single write + sync */
	uint64_t free_bytes;
	uint64_t total_bytes;
};

/** Start the writer thread: it mounts the file system and starts logging. */
void datalog_start(void);

/** Log a free-text note (shell "datalog note"), e.g. to mark a test step. */
void datalog_note(const char *text);

/**
 * Write the buffered lines to flash now and wait (up to timeout_ms) until
 * they're synced, so a reader sees everything logged so far.
 */
int datalog_flush(int timeout_ms);

/** Delete every log file and start a new one. */
int datalog_clear(int timeout_ms);

void datalog_get_stats(struct datalog_stats *stats);

/** Log files, oldest first. Returns how many were stored (up to max), or -errno. */
int datalog_list(struct datalog_file *files, int max);

/**
 * The last `lines` lines of the file being written, as a NUL-terminated
 * string. With flush, buffered lines are written first; without, it shows
 * what was synced (at most CONFIG_APP_DATALOG_SYNC_S old) and causes no
 * flash writes. Returns its length, or -errno.
 */
int datalog_tail(char *out, size_t len, int lines, bool flush);

/**
 * Full path of a log file from its name ("log00042.csv"). Rejects anything
 * else, so a name from the network can't reach other paths. -EINVAL if bad.
 */
int datalog_path(const char *name, char *path, size_t len);

#endif /* DATALOG_H_ */
