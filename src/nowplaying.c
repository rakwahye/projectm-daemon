// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file nowplaying.c
 * @brief Poll loop and track-change detection.
 *
 * Diffs a cached track key each poll and, on change, shells out for
 * metadata and art. */

#define _GNU_SOURCE

#include "nowplaying.h"
#include "info_views.h"
#include "runtime.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

static pthread_t g_thread;
static _Atomic int s_run = 0; // worker run flag
static int g_poll_ms;

/* Cached last-seen track key for change detection */
static char g_last_key[1024]; // "artist|title"
static char g_last_art_url[1024];
static char g_local_art_path[512]; // the file we wrote art to

/* Run a command, capture stdout into buf. Returns 0 on success.
 * Trims trailing newlines. */
static int run_cmd(const char *cmd, char *buf, size_t buflen) {
	buf[0] = '\0';
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	size_t total = 0;
	while (total < buflen - 1) {
		size_t n = fread(buf + total, 1, buflen - 1 - total, p);
		if (n == 0) break;
		total += n;
	}
	buf[total] = '\0';
	int rc = pclose(p);
	/* Trim trailing newlines and whitespace */
	while (total > 0 && (buf[total-1] == '\n' || buf[total-1] == '\r'
		               || buf[total-1] == ' ' || buf[total-1] == '\t'))
		buf[--total] = '\0';
	return rc;
}

/* Check if a command exists in PATH */
static int command_exists(const char *cmd) {
	char check[256];
	snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", cmd);
	return system(check) == 0;
}

/* Query a single metadata field via playerctl. Returns 1 on success. */
static int playerctl_field(const char *field, char *out, size_t outlen) {
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		"playerctl metadata --format '{{%s}}' 2>/dev/null", field);
	if (run_cmd(cmd, out, outlen) != 0) {
		out[0] = '\0';
		return 0;
	}
	return out[0] ? 1 : 0;
}

/* Get playerctl status. Playing, Paused, Stopped, or empty */
static int playerctl_status(char *out, size_t outlen) {
	if (run_cmd("playerctl status 2>/dev/null", out, outlen) != 0) {
		out[0] = '\0';
		return 0;
	}
	return out[0] ? 1 : 0;
}

/* Strip "file://" prefix if present. */
static const char *strip_file_url(const char *url) {
	if (strncmp(url, "file://", 7) == 0) return url + 7;
	return url;
}

/* Download a remote URL to /tmp/nowplaying-art.<ext>. Returns the
 * local path on success, NULL on failure. */
static const char *fetch_remote_art(const char *url) {
	if (!command_exists("curl")) return NULL;

	/* Guess extension from URL */
	const char *ext = ".jpg";
	const char *q = strchr(url, '?');
	int url_end = q ? (int)(q - url) : (int)strlen(url);
	for (int i = url_end - 1; i >= 0 && url_end - i < 8; i--) {
		if (url[i] == '.') {
		    if (!strncasecmp(url + i, ".png", 4)) ext = ".png";
		    else if (!strncasecmp(url + i, ".jpg", 4)) ext = ".jpg";
		    else if (!strncasecmp(url + i, ".jpeg", 5)) ext = ".jpg";
		    break;
		}
	}

	snprintf(g_local_art_path, sizeof(g_local_art_path),
	         "/tmp/nowplaying-art%s", ext);

	/* Remove any prior file so a failed fetch doesn't show stale art */
	unlink(g_local_art_path);

	char cmd[2048];
	/* Quote the URL safely. Replace any "'" in URL with '"'"' is overkill,
	 * but URLs from playerctl shouldn't contain single quotes. Bail if
	 * it does. */
	if (strchr(url, '\'')) return NULL;
	snprintf(cmd, sizeof(cmd),
	         "curl -sL --max-time 10 -o '%s' '%s' >/dev/null 2>&1",
	         g_local_art_path, url);
	int rc = system(cmd);
	if (rc != 0) return NULL;

	/* Verify file exists and is non-trivial */
	struct stat st;
	if (stat(g_local_art_path, &st) != 0 || st.st_size < 64) return NULL;
	return g_local_art_path;
}

/* Resolve a playerctl artUrl into a local file path. Returns NULL if no art. */
static const char *resolve_art(const char *url) {
	if (!url || !url[0]) return NULL;
	if (strncmp(url, "file://", 7) == 0)
		return strip_file_url(url);
	if (strncmp(url, "http://", 7) == 0 ||
		strncmp(url, "https://", 8) == 0)
		return fetch_remote_art(url);
	/* Maybe it's already a local path */
	if (url[0] == '/') return url;
	return NULL;
}

static void poll_once(void) {
	char status[64];
	if (!playerctl_status(status, sizeof(status))) {
		/* No player or playerctl missing. Clear slot if needed */
		if (g_last_key[0]) {
			info_now_playing("");
			info_now_playing_art(NULL);
			g_last_key[0] = '\0';
			g_last_art_url[0] = '\0';
		}
		return;
	}

	/* Paused or Stopped: Leave whatever is there alone. Feels less jittery
	 * than wiping the slot every time you pause. Or clear on stop only. */
	if (strcmp(status, "Stopped") == 0) {
		if (g_last_key[0]) {
			info_now_playing("");
			info_now_playing_art(NULL);
			g_last_key[0] = '\0';
			g_last_art_url[0] = '\0';
		}
		return;
	}

	char artist[256], title[512], album[256];
	playerctl_field("xesam:artist", artist, sizeof(artist));
	playerctl_field("xesam:title", title, sizeof(title));
	playerctl_field("xesam:album", album, sizeof(album));

	if (!title[0] && !artist[0]) {
		/* No metadata. If we had something before, that's a real
		 * transition, clear the slot rather than leaving stale text
		 * on screen. */
		if (g_last_key[0]) {
			info_now_playing("");
			info_now_playing_art(NULL);
			g_last_key[0] = '\0';
			g_last_art_url[0] = '\0';
		}
		return;
	}

	char key[1024];
	snprintf(key, sizeof(key), "%s|%s", artist, title);
	if (strcmp(key, g_last_key) == 0) return; // no change
	snprintf(g_last_key, sizeof(g_last_key), "%s", key);

	char text[1024];
	if (artist[0] && title[0])
		snprintf(text, sizeof(text), "%s\n%s", title, artist);
	else if (title[0])
		snprintf(text, sizeof(text), "%s", title);
	else
		snprintf(text, sizeof(text), "%s", artist);

	info_now_playing(text);

	/* Album art. Push on every track change, not just URL change. Different
	 * tracks on the same album share an artUrl, so URL-only detection skips
	 * art on track 2 of an album. But, don't re-curl the same URL, since
	 * resolve_art always downloads. Only fetch when the URL actually changed.
	 * Use the cached local path on subsequent tracks of the same album. */
	char art_url[1024];
	playerctl_field("mpris:artUrl", art_url, sizeof(art_url));
	if (!art_url[0]) {
		/* No art for this track. Clear. */
		info_now_playing_art(NULL);
		g_last_art_url[0] = '\0';
	} else if (strcmp(art_url, g_last_art_url) != 0) {
		/* URL changed. Fetch and push. */
		snprintf(g_last_art_url, sizeof(g_last_art_url), "%s", art_url);
		const char *local = resolve_art(art_url);
		info_now_playing_art(local); // NULL if resolve fails
	} else if (g_local_art_path[0]) {
		/* Same URL as last track. Reuse the already-fetched local file.
		 * Re-push so the slot's FLASH gets art rendered this round too. */
		info_now_playing_art(g_local_art_path);
	} else {
		/* Same URL but no local cache (file:// path or unsupported
		 * scheme). Resolve fresh. NULL for unsupported. */
		const char *local = resolve_art(art_url);
		info_now_playing_art(local);
	}

	fprintf(stderr, "[nowplaying] %s\n", key);
}

static void *worker(void *arg) {
	(void)arg;

	if (!command_exists("playerctl")) {
		fprintf(stderr, "[nowplaying] playerctl not found, disabled\n");
		return NULL;
	}

	/* Initial small delay so the daemon settles first */
	struct timespec start_delay = { 1, 0 };
	nanosleep(&start_delay, NULL);

	while (atomic_load(&s_run)) {
		poll_once();

		/* Sleep in small slices so stop is responsive */
		int slept = 0;
		while (atomic_load(&s_run) && slept < g_poll_ms) {
			int chunk = (g_poll_ms - slept > 200) ? 200 : (g_poll_ms - slept);
			struct timespec ts = { chunk / 1000, (chunk % 1000) * 1000000L };
			nanosleep(&ts, NULL);
			slept += chunk;
		}
	}
	return NULL;
}

int nowplaying_start(struct rt *rt) {
	(void)rt;
	g_poll_ms = 3000;
	atomic_store(&s_run, 1);
	if (pthread_create(&g_thread, NULL, worker, NULL) != 0) {
		fprintf(stderr, "[nowplaying] pthread_create failed: %s\n", strerror(errno));
		atomic_store(&s_run, 0);
		return 0;
	}
	return 1;
}

void nowplaying_stop(void) {
	if (!atomic_load(&s_run)) return;
	atomic_store(&s_run, 0);
	pthread_join(g_thread, NULL);
	/* Clean up temp art file */
	if (g_local_art_path[0]) unlink(g_local_art_path);
}

/* Failed watcher thread is non-fatal, so the hook always reports
 * success and never aborts startup. */
static int nowplaying_module_init(struct rt *rt) {
	nowplaying_start(rt);
	return 1;
}

MODULE_REGISTER(nowplaying,
	.init = nowplaying_module_init,
	.shutdown = nowplaying_stop);
