// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file info_views.c
 * @brief Field stack and HUD composition.
 *
 * Tracks each field's mode and flash timing and composes the visible
 * stack into one string for the overlay per frame. */

#define _POSIX_C_SOURCE 200809L

#include "module_registry.h"
#include "info_views.h"
#include "overlay.h"
#include "playlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>

/* Per-field display mode. */
enum info_mode {
	INFO_MODE_OFF = 0,
	INFO_MODE_FLASH = 1,
	INFO_MODE_PERSISTENT = 2,
};

/* Published field state. */
static pthread_mutex_t s_fields_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	char now_playing[1024];
	char now_playing_art[1024];
	char preset[512];
	char performance[256];
	struct timespec np_set_time; // now-playing last-changed (flash)
	struct timespec preset_set_time; // preset last-changed (flash)
} s_fields;

/* The last view pushed to overlay. Touched only by info_views_tick.
 * Lets us push to overlay only when the composite actually changes,
 * instead of every frame. */
static char s_last_view[4096];
static int s_last_show_art = -1;

void info_now_playing(const char *text) {
	const char *t = text ? text : "";
	pthread_mutex_lock(&s_fields_lock);
	if (strcmp(t, s_fields.now_playing) != 0) {
		snprintf(s_fields.now_playing, sizeof(s_fields.now_playing), "%s", t);
		clock_gettime(CLOCK_MONOTONIC, &s_fields.np_set_time); // restart flash
	}
	pthread_mutex_unlock(&s_fields_lock);
}
void info_now_playing_art(const char *art_path) {
	pthread_mutex_lock(&s_fields_lock);
	snprintf(s_fields.now_playing_art, sizeof(s_fields.now_playing_art), "%s",
		     art_path ? art_path : "");
	pthread_mutex_unlock(&s_fields_lock);
	/* Cache the bitmap in overlay now, decoupled from HUD visibility,
	 * so the peek can show it after now-playing flashes out. */
	overlay_set_art(art_path);
}
void info_preset(const char *text) {
	const char *t = text ? text : "";
	pthread_mutex_lock(&s_fields_lock);
	if (strcmp(t, s_fields.preset) != 0) {
		snprintf(s_fields.preset, sizeof(s_fields.preset), "%s", t);

		/* restart flash */
		clock_gettime(CLOCK_MONOTONIC, &s_fields.preset_set_time);
	}
	pthread_mutex_unlock(&s_fields_lock);
}
void info_performance(const char *text) {
	pthread_mutex_lock(&s_fields_lock);
	snprintf(s_fields.performance, sizeof(s_fields.performance), "%s", text ? text : "");
	pthread_mutex_unlock(&s_fields_lock);
}

/* Config slice */
static struct {
	unsigned peek;
	int nowplaying_mode; // enum info_mode
	int preset_mode; // enum info_mode
	int nowplaying_art; // 0/1 toggle
	int flash_duration; // ms
} s_cfg;

/* Which fields the `info` peek shows. */
#define INFO_FIELD_ART (1u << 0)
#define INFO_FIELD_NOWPLAYING (1u << 1)
#define INFO_FIELD_PRESET (1u << 2)
#define INFO_FIELD_PERFORMANCE (1u << 3)
#define INFO_FIELD_SHUFFLE (1u << 4)
#define INFO_FIELD_LOCK (1u << 5)
#define INFO_PEEK_DEFAULT (INFO_FIELD_ART | INFO_FIELD_NOWPLAYING | INFO_FIELD_PRESET)

static unsigned parse_peek(const char *val) {
	unsigned bits = 0;
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", val);
	char *saveptr = NULL;
	char *tok = strtok_r(buf, ", \t", &saveptr);
	while (tok) {
		if (!strcasecmp(tok, "art")) bits |= INFO_FIELD_ART;
		else if (!strcasecmp(tok, "nowplaying")) bits |= INFO_FIELD_NOWPLAYING;
		else if (!strcasecmp(tok, "preset")) bits |= INFO_FIELD_PRESET;
		else if (!strcasecmp(tok, "performance")) bits |= INFO_FIELD_PERFORMANCE;
		else if (!strcasecmp(tok, "shuffle")) bits |= INFO_FIELD_SHUFFLE;
		else if (!strcasecmp(tok, "lock")) bits |= INFO_FIELD_LOCK;
		else if (!strcasecmp(tok, "none") || !strcasecmp(tok, "off")) bits = 0;
		else fprintf(stderr, "[config] unknown info.peek token: %s\n", tok);
		tok = strtok_r(NULL, ", \t", &saveptr);
	}
	return bits;
}

static void info_config_defaults(void) {
	s_cfg.peek = INFO_PEEK_DEFAULT;
	s_cfg.nowplaying_mode = INFO_MODE_FLASH;
	s_cfg.preset_mode = INFO_MODE_OFF;
	s_cfg.nowplaying_art = 1;
	s_cfg.flash_duration = 4000;
}

static int info_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "peek")) { s_cfg.peek = parse_peek(val); return 1; }
	if (!strcmp(k, "nowplaying_mode")) { s_cfg.nowplaying_mode = atoi(val); return 1; }
	if (!strcmp(k, "preset_mode")) { s_cfg.preset_mode = atoi(val); return 1; }
	if (!strcmp(k, "nowplaying_art")) { s_cfg.nowplaying_art = atoi(val); return 1; }
	if (!strcmp(k, "flash_duration")) { s_cfg.flash_duration = atoi(val); return 1; }
	return 0;
}

/* Is a field currently on screen? Mode, text presence, and flash timer.
 * PERSISTENT ignores the timer. FLASH shows until flash_ms elapsed. */
static int field_visible(int mode, const char *text,
		         const struct timespec *set_time, int flash_ms) {
	if (mode == INFO_MODE_OFF) return 0;
	if (!text[0]) return 0;
	if (mode == INFO_MODE_PERSISTENT) return 1;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed = (now.tv_sec - set_time->tv_sec) * 1000.0
		          + (now.tv_nsec - set_time->tv_nsec) / 1e6;
	return elapsed < flash_ms;
}

void info_views_tick(void) {
	/* Snapshot fields under the lock. Compose lock-free. Avoids nesting
	 * s_fields_lock under overlay's request lock. */
	char np[1024], pr[512], pf[256];
	int have_art;
	struct timespec np_t, pr_t;
	pthread_mutex_lock(&s_fields_lock);
	snprintf(np, sizeof(np), "%s", s_fields.now_playing);
	snprintf(pr, sizeof(pr), "%s", s_fields.preset);
	snprintf(pf, sizeof(pf), "%s", s_fields.performance);
	have_art = s_fields.now_playing_art[0] ? 1 : 0;
	np_t = s_fields.np_set_time;
	pr_t = s_fields.preset_set_time;
	pthread_mutex_unlock(&s_fields_lock);

	int flash_ms = s_cfg.flash_duration > 0 ? s_cfg.flash_duration : 4000;
	int np_vis = field_visible(s_cfg.nowplaying_mode, np, &np_t, flash_ms);
	int pr_vis = field_visible(s_cfg.preset_mode, pr, &pr_t, flash_ms);
	int pf_vis = pf[0] ? 1 : 0;

	/* Stack order matches the historical slot view. */
	char view[4096];
	int off = 0, first = 1;
	#define VIEW_APPEND(s) do { \
		if (!first) off += snprintf(view + off, sizeof(view) - off, "\n"); \
		off += snprintf(view + off, sizeof(view) - off, "%s", (s)); \
		first = 0; \
	} while (0)
	if (pf_vis) VIEW_APPEND(pf);
	if (pr_vis) VIEW_APPEND(pr);
	if (np_vis) VIEW_APPEND(np);
	#undef VIEW_APPEND
	if (first) view[0] = '\0';

	int show_art = (s_cfg.nowplaying_art && np_vis && have_art) ? 1 : 0;

	/* Push only on change so steady state doesn't restart the fade. */
	if (show_art != s_last_show_art || strcmp(view, s_last_view) != 0) {
		overlay_set_view(view, show_art);
		snprintf(s_last_view, sizeof(s_last_view), "%s", view);
		s_last_show_art = show_art;
	}
}

/* Socket reply. The textual state dump. Effect - the on-screen peek,
 * composed from the info.peek field mask, info_views' published fields,
 * and playlist state. Peek render order: NOWPLAYING, PRESET, PERFORMANCE,
 * SHUFFLE, LOCK. Each appear if its field bit is set AND has content. */
static int info_ipc_command(struct ipc_command_ctx *c) {
	struct playlist_view v;
	playlist_snapshot_get(&v);
	const char *current = v.current_path[0] ? v.current_path : "(none)";
	int shuffle = v.is_shuffled;
	int locked = v.is_locked;
	int pos = v.index + 1; // File-order slot

	snprintf(c->reply, c->reply_len,
		"playlist: %s\n"
		"preset: %s\n"
		"position: %d/%d\n"
		"shuffle: %s\n"
		"lock: %s\n"
		"duration: %.0fs\n",
		v.name,
		current,
		pos, v.count,
		shuffle ? "on" : "off",
		locked ? "on" : "off",
		playlist_auto_advance_seconds());

	unsigned bits = s_cfg.peek;
	char peek[2048];
	int poff = 0;

	/* Snapshot fields under the lock. Compose lock-free. Avoids nesting
	 * s_fields_lock under overlay's request lock. */
	char np[1024], pr[512], pf[256];
	pthread_mutex_lock(&s_fields_lock);
	snprintf(np, sizeof(np), "%s", s_fields.now_playing);
	snprintf(pr, sizeof(pr), "%s", s_fields.preset);
	snprintf(pf, sizeof(pf), "%s", s_fields.performance);
	pthread_mutex_unlock(&s_fields_lock);

	#define PEEK_APPEND(fmt, ...) do { \
		if (poff < (int)sizeof(peek)) { \
		    if (poff) poff += snprintf(peek + poff, sizeof(peek) - poff, "\n"); \
		    poff += snprintf(peek + poff, sizeof(peek) - poff, fmt, ##__VA_ARGS__); \
		} \
	} while (0)

	if ((bits & INFO_FIELD_NOWPLAYING) && np[0]) PEEK_APPEND("%s", np);
	if ((bits & INFO_FIELD_PRESET) && pr[0]) PEEK_APPEND("%s", pr);
	if ((bits & INFO_FIELD_PERFORMANCE) && pf[0]) PEEK_APPEND("%s", pf);
	if (bits & INFO_FIELD_SHUFFLE) PEEK_APPEND("shuffle: %s", shuffle ? "on" : "off");
	if (bits & INFO_FIELD_LOCK) PEEK_APPEND("lock: %s", locked ? "on" : "off");
	if (poff == 0) snprintf(peek, sizeof(peek), "(empty)");

	#undef PEEK_APPEND

	overlay_show_info(peek, (bits & INFO_FIELD_ART) ? 1 : 0);
	return 0;
}

MODULE_REGISTER(info_views,
	.config_prefix = "info",
	.config_template =
		"info.peek=art,nowplaying,preset\n"
		"info.nowplaying_mode=1   # integer. 0 = off, 1 = flash, 2 = always on\n"
		"info.preset_mode=0   # integer. 0 = off, 1 = flash, 2 = always on\n"
		"info.nowplaying_art=1   # boolean\n"
		"info.flash_duration=4000   # milliseconds\n",
	.config_defaults = info_config_defaults,
	.config_parse = info_config_parse,
	.ipc_verb = "info",
	.ipc_command = info_ipc_command,
	.ipc_help = "\ninfo                 show current daemon state\n");
