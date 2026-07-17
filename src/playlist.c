// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file playlist.c
 * @brief `playlist` config slice and list state.
 *
 * Builds and orders the preset list, tracks blacklists and history, and
 * advances by age or command. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "app_paths.h"
#include "config.h"
#include "presets.h"
#include "module_registry.h"
#include "overlay.h"
#include "info_views.h"
#include "runtime.h"
#include "visualizer.h"
#include "playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>

extern int g_debug;
#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

static struct playlist_config s_cfg;

static void playlist_config_defaults(void) {
	s_cfg.shuffle = 1;
	s_cfg.auto_load[0] = '\0';
	s_cfg.duration = 30.0;
}

static int playlist_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "shuffle")) { s_cfg.shuffle = atoi(val); return 1; }
	if (!strcmp(k, "duration")) { s_cfg.duration = atof(val); return 1; }
	if (!strcmp(k, "auto_load")) {
		snprintf(s_cfg.auto_load, sizeof(s_cfg.auto_load), "%s", val);
		return 1;
	}
	return 0;
}

/* Start at the top of the list, shuffling first when shuffle is on. */
static void playlist_seek_start(void) {
	if (playlist_count() <= 0) return;
	if (playlist_is_shuffled()) playlist_shuffle();
	else playlist_set_idx(0);
	playlist_load_current(true);
}

/* Copy the pending payload out under lock and consume it. Byte-copied, not
 * string-copied: PENDING_PLAYLIST_NEW packs two strings as `a\0b` in the one
 * buffer. Returns 0 when no payload is waiting. */
static int playlist_pending_take_path(struct rt *rt, char *out, size_t outsz) {
	pthread_mutex_lock(&rt->pending.lock);
	int have = rt->pending.path[0] != '\0';
	if (have) {
		size_t n = sizeof(rt->pending.path);
		if (n > outsz) n = outsz;
		memcpy(out, rt->pending.path, n);
		out[outsz - 1] = '\0';
		rt->pending.path[0] = '\0';
	}
	pthread_mutex_unlock(&rt->pending.lock);
	return have;
}

static double playlist_pending_take_duration(struct rt *rt) {
	pthread_mutex_lock(&rt->pending.lock);
	double d = rt->pending.duration;
	pthread_mutex_unlock(&rt->pending.lock);
	return d;
}

/* Prefer a sibling .autosave when one exists on disk, so a load restores the
 * last session. Source mirrors never have one, autosave being suppressed for
 * them. Returns the path to load, either `canonical` or `ap`. */
static const char *playlist_autosave_prefer(const char *canonical, char *ap, size_t apsz,
                                   int *out_from_autosave)
{
	struct stat st;
	*out_from_autosave = 0;
	if (!playlist_autosave_path_for(canonical, ap, (int)apsz)) return canonical;
	if (stat(ap, &st) != 0) return canonical;

	*out_from_autosave = 1;
	return ap;
}

/* Strip the playlists dir and the .lst suffix, so a playlist that lives in the
 * standard place is stored under its bare name. Anything outside keeps its
 * full path. */
static void playlist_auto_load_value_for(const char *path, char *out, size_t outsz) {
	char prefix[1024];
	snprintf(prefix, sizeof(prefix), "%s/", app_paths_playlists_dir());
	size_t plen = strlen(prefix);

	if (strncmp(path, prefix, plen) != 0) {
		snprintf(out, outsz, "%s", path);
		return;
	}

	snprintf(out, outsz, "%s", path + plen);
	char *dot = strrchr(out, '.');
	if (dot && strcmp(dot, ".lst") == 0) *dot = '\0';
}

/* Persist what we just loaded, so the next boot restores it. */
static void playlist_persist_auto_load(const char *path) {
	if (!config_active_path()[0]) return;

	char value[1024];
	playlist_auto_load_value_for(path, value, sizeof(value));

	if (config_set_key(config_active_path(), "playlist.auto_load", value))
		DBG("[config] set playlist_auto_load=%s", value);
	else
		DBG("[config] set playlist_auto_load failed");
}

static void playlist_pending_load(const char *path) {
	char ap[1024];
	int from_autosave;
	const char *load_path = playlist_autosave_prefer(path, ap, sizeof(ap),
	                                        &from_autosave);

	if (!playlist_load_file(load_path)) {
		LOG("[playlist] load FAILED: %s: %s", load_path, strerror(errno));
		return;
	}

	playlist_set_name_from_path(path);
	playlist_set_src_path(path);
	playlist_access_log_append(playlist_name());

	if (from_autosave) {
		DBG("[playlist] loaded '%s' from autosave (%d presets)",
		    playlist_name(), playlist_count());
		struct overlay_spec d;
		overlay_spec_init(&d);
		snprintf(d.text, sizeof(d.text),
		         "%s: restored from autosave", playlist_name());
		d.duration_ms = 4000;
		overlay_show(&d);
	} else {
		DBG("[playlist] loaded '%s' from %s (%d presets)",
		    playlist_name(), path, playlist_count());
	}

	if (playlist_count() > 0) playlist_load_current(true);
	else LOG("[playlist] %s loaded but contains no valid preset paths",
	         load_path);

	playlist_persist_auto_load(path);
}

static void playlist_pending_save(const char *path) {
	if (!playlist_save_file(path)) {
		LOG("[playlist] save FAILED: %s: %s", path, strerror(errno));
		return;
	}
	DBG("[playlist] saved to %s (%d presets)", path, playlist_count());

	playlist_clear_src_path();
	playlist_set_src_path(path);
	playlist_set_name_from_path(path);

	/* The sibling autosave describes an older state now. */
	char ap[1024];
	if (playlist_autosave_path_for(path, ap, sizeof(ap)) && unlink(ap) == 0)
		DBG("[autosave] removed stale %s", ap);
}

/* `payload` is the pre-validated `name\0dir` pair from IPC. An empty dir means
 * an empty playlist. */
static void playlist_pending_new(const char *payload) {
	const char *name = payload;
	const char *dir = payload + strlen(payload) + 1;

	playlist_reset();
	playlist_set_name(name);

	if (dir[0]) {
		struct stat st;
		if (stat(dir, &st) != 0)
			LOG("[playlist] new: cannot stat %s: %s", dir, strerror(errno));
		else if (S_ISDIR(st.st_mode))
			playlist_scan_directory(dir);
		else
			playlist_load_file(dir);
	}

	DBG("[playlist] new '%s' (%d presets)", playlist_name(), playlist_count());
	playlist_seek_start();
}

static void playlist_pending_merge(const char *path) {
	int before = playlist_count();
	if (!playlist_merge_file(path)) {
		LOG("[playlist] merge FAILED: %s", path);
		return;
	}
	DBG("[playlist] merged from %s (+%d presets, total %d)",
	    path, playlist_count() - before, playlist_count());
	playlist_autosave_current();
}

static void playlist_pending_rescan(void) {
	playlist_reset();
	for (int ri = 0; ri < presets_dir_count(); ri++)
		playlist_scan_directory(presets_dir_at(ri));
	DBG("[playlist] rescanned %s (%d presets)",
	    presets_dir(), playlist_count());
	playlist_seek_start();
}

static void playlist_pending_add(const char *target, int prepend) {
	/* The live current preset is read on this thread, never off it. */
	const char *preset = playlist_current_path();
	if (!preset) return;

	char err[256] = {0};
	if (playlist_add_to_file(target, preset, prepend, err, sizeof(err)))
		DBG("[playlist-add%s] %s ← %s",
		    prepend ? "-pin" : "", target, preset);
	else
		LOG("[playlist-add%s] %s FAILED: %s",
		    prepend ? "-pin" : "", target, err);
}

static void playlist_pending_pin(void) {
	/* Reorders the array in place, so it has to run on the render thread.
	 * Only rc 2 means the order actually moved. */
	if (playlist_pin_current_to_top() != 2) return;
	DBG("[playlist-pin] reordered: running preset is now at index 0");
	if (playlist_src_path()[0]) playlist_autosave_current();
}

static void playlist_pending_set_duration(struct rt *rt) {
	playlist_set_auto_advance(playlist_pending_take_duration(rt));
	playlist_set_locked(0);
	playlist_reset_preset_timer();
	DBG("[ipc] dur: %.0fs (unlocked)", playlist_auto_advance_seconds());
}

/* The pending payload is consumed before the handler runs, so no handler holds
 * `rt->pending.lock` across file I/O or an overlay call. */
void playlist_apply_pending(struct rt *rt, int action) {
	char path[sizeof(((struct rt *)0)->pending.path)];

	switch (action) {
	case PENDING_NEXT:
		playlist_next();
		playlist_load_current(true);
		break;
	case PENDING_PREV:
		playlist_prev();
		playlist_load_current(true);
		break;
	case PENDING_SNAP:
		playlist_next();
		playlist_load_current(false);
		break;
	case PENDING_BLACKLIST:
		playlist_blacklist_current(0, NULL, 0);
		break;
	case PENDING_BLACKLIST_GLOBAL:
		playlist_blacklist_current(1, NULL, 0);
		break;
	case PENDING_UNDO:
		playlist_undo_blacklist(NULL, 0, NULL);
		break;

	case PENDING_PLAYLIST_LOAD:
		if (playlist_pending_take_path(rt, path, sizeof(path))) playlist_pending_load(path);
		break;
	case PENDING_PLAYLIST_SAVE:
		if (playlist_pending_take_path(rt, path, sizeof(path))) playlist_pending_save(path);
		break;
	case PENDING_PLAYLIST_NEW:
		if (playlist_pending_take_path(rt, path, sizeof(path))) playlist_pending_new(path);
		break;
	case PENDING_PLAYLIST_MERGE:
		if (playlist_pending_take_path(rt, path, sizeof(path))) playlist_pending_merge(path);
		break;
	case PENDING_PLAYLIST_ADD:
	case PENDING_PLAYLIST_ADD_PIN:
		if (playlist_pending_take_path(rt, path, sizeof(path)))
			playlist_pending_add(path, action == PENDING_PLAYLIST_ADD_PIN);
		break;

	case PENDING_PLAYLIST_RESCAN:
		playlist_pending_rescan();
		break;
	case PENDING_PLAYLIST_PIN:
		playlist_pending_pin();
		break;
	case PENDING_SET_DURATION:
		playlist_pending_set_duration(rt);
		break;

	case PENDING_TOGGLE_SHUFFLE: {
		int on = !playlist_is_shuffled();
		playlist_set_shuffled(on);
		DBG("[ipc] shuffle: %s", on ? "on" : "off");
		break;
	}
	case PENDING_TOGGLE_LOCK: {
		int on = !playlist_is_locked();
		playlist_set_locked(on);
		DBG("[ipc] lock: %s", on ? "on" : "off");
		break;
	}
	default:
		break;
	}
}

static const char *match_pl_verb(const char *line, const char *subverb) {
	const char *rest;
	if (strncmp(line, "playlist", 8) == 0 &&
	    (line[8] == ' ' || line[8] == '\t')) {
		rest = line + 8;
	} else if (strncmp(line, "pl", 2) == 0 &&
	           (line[2] == ' ' || line[2] == '\t')) {
		rest = line + 2;
	} else {
		return NULL;
	}
	while (*rest == ' ' || *rest == '\t') rest++;

	size_t sub_len = strlen(subverb);
	if (strncmp(rest, subverb, sub_len) != 0) return NULL;
	if (rest[sub_len] != ' ' && rest[sub_len] != '\t' && rest[sub_len] != '\0')
		return NULL;

	rest += sub_len;
	while (*rest == ' ' || *rest == '\t') rest++;
	return rest;
}

static int resolve_playlist_path(const char *input, char *out, size_t outlen) {
	if (!input || !input[0]) return 0;

	if (strchr(input, '/')) {
		snprintf(out, outlen, "%s", input);
		return 1;
	}

	/* Bare name -> <config_dir>/playlists/<name>.lst (app_paths owns the dir). */
	const char *dot = strrchr(input, '.');
	int has_lst_ext = dot && strcmp(dot, ".lst") == 0;
	snprintf(out, outlen, "%s/playlists/%s%s",
	         app_paths_config_dir(), input, has_lst_ext ? "" : ".lst");
	return 1;
}

static int playlist_ipc_list_presets(char *buf, int buflen) {
	int off = 0;
	int cur = 0;
	int count = playlist_snapshot_lock(&cur);
	for (int i = 0; i < count && off < buflen - 2; i++) {
		const char *p = playlist_snapshot_entry(i);
		int n = snprintf(buf + off, buflen - off, "%s%s\n",
		                 (i == cur) ? "* " : "  ", p ? p : "");
		if (n < 0 || n >= buflen - off) { off = buflen - 1; break; }
		off += n;
	}
	playlist_snapshot_unlock();
	return off;
}

static int playlist_ipc_blacklist(struct rt *mbox, char *buf, int buflen) {
	struct playlist_view v;
	playlist_snapshot_get(&v);
	if (!v.current_path[0]) {
		return snprintf(buf, buflen, "err nothing to blacklist\n");
	}
	if (v.is_source) {
		return snprintf(buf, buflen,
		    "err cannot blacklist on an autogenerated playlist, "
		    "did you mean blacklist-global?\n");
	}
	int n = snprintf(buf, buflen, "ok blacklisted: %s\n",
	                 path_basename(v.current_path));
	rt_pending_post(mbox, PENDING_BLACKLIST);
	return n;
}

static int playlist_ipc_blacklist_global(struct rt *mbox, char *buf, int buflen) {
	struct playlist_view v;
	playlist_snapshot_get(&v);
	if (!v.current_path[0]) {
		return snprintf(buf, buflen, "err nothing to blacklist\n");
	}
	int n = snprintf(buf, buflen,
	                 "ok blacklisted globally: %s\n",
	                 path_basename(v.current_path));
	rt_pending_post(mbox, PENDING_BLACKLIST_GLOBAL);
	return n;
}

static int playlist_ipc_undo(struct rt *mbox, char *buf, int buflen) {
	struct playlist_view v;
	playlist_snapshot_get(&v);
	if (!v.undo_basename[0]) {
		return snprintf(buf, buflen, "err nothing to undo\n");
	}
	int n = snprintf(buf, buflen, "ok restored: %s%s\n",
	                 v.undo_basename, v.undo_lane_label);
	rt_pending_post(mbox, PENDING_UNDO);
	return n;
}

static int playlist_ipc_save(struct rt *mbox, const char *n,
                             char *reply_buf, int reply_buflen) {
	struct playlist_view v;
	playlist_snapshot_get(&v);

	/* Compute target path. Both forms converge here. IPC passes the new
	 * path or "" to use current target. */
	char target[1024];
	if (n && n[0]) {
		snprintf(target, sizeof(target), "%s", n);
	} else {
		playlist_path_for_name(v.name, target, sizeof(target));
	}

	/* Source-overwrite guard. If the currently-loaded playlist is an auto-
	 * generated source mirror and the requested target is the same file,
	 * refuse. */
	if (v.is_source && v.src_path[0] && strcmp(target, v.src_path) == 0) {
		return snprintf(reply_buf, reply_buflen,
		    "err source playlist; save under a new name with: playlist-save <name>\n");
	}

	/* Queue the save. Main thread picks it up and runs playlist_save_file,
	 * which writes a plain user-owned playlist. After save succeeds the
	 * PL_SAVE handler clears the is_source flag. */
	rt_pending_post_path(mbox, PENDING_PLAYLIST_SAVE, target);

	return snprintf(reply_buf, reply_buflen,
	                "ok playlist saved: %s\n", path_basename(target));
}

static int playlist_ipc_add(struct rt *mbox, const char *name, int prepend,
                            char *err_buf, int err_buflen) {
	struct playlist_view v;
	playlist_snapshot_get(&v);
	if (!v.current_path[0]) {
		snprintf(err_buf, err_buflen, "no running preset");
		return 0;
	}
	rt_pending_post_path(mbox, prepend ? PENDING_PLAYLIST_ADD_PIN : PENDING_PLAYLIST_ADD, name);
	return 1;
}

static void playlist_ipc_new(struct rt *mbox, const char *name, const char *dir) {
	rt_pending_post_pair(mbox, PENDING_PLAYLIST_NEW, name, dir);
}

static void playlist_ipc_merge(struct rt *mbox, const char *p) {
	rt_pending_post_path(mbox, PENDING_PLAYLIST_MERGE, p);
}

static void playlist_ipc_load(struct rt *mbox, const char *p) {
	rt_pending_post_path(mbox, PENDING_PLAYLIST_LOAD, p);
}

static int playlist_ipc_pin(struct rt *mbox, char *err_buf, int err_buflen) {
	struct playlist_view v;
	playlist_snapshot_get(&v);
	if (!v.current_path[0]) {
		snprintf(err_buf, err_buflen, "no running preset");
		return 0;
	}
	if (v.index == 0) {
		snprintf(err_buf, err_buflen, "already at top");
		return 1; // not an error - no-op success
	}
	/* Defer rotation to render thread. */
	rt_pending_post(mbox, PENDING_PLAYLIST_PIN);
	if (v.src_path[0])
		snprintf(err_buf, err_buflen, "pinned (autosaved)");
	else
		snprintf(err_buf, err_buflen, "pinned (in-memory only)");
	return 1;
}

int playlist_ipc_command(struct rt *mbox, const char *line,
                         char *reply, int reply_len) {
	const char *pl_arg;
	(void)pl_arg;
	if (strcmp(line, "next") == 0) {
		if (playlist_is_locked()) {
			snprintf(reply, reply_len, "err locked\n");
		} else {
			rt_pending_post(mbox, PENDING_NEXT);
			snprintf(reply, reply_len, "ok next\n");
		}

	} else if (strcmp(line, "prev") == 0) {
		if (playlist_is_locked()) {
			snprintf(reply, reply_len, "err locked\n");
		} else {
			rt_pending_post(mbox, PENDING_PREV);
			snprintf(reply, reply_len, "ok prev\n");
		}

	} else if (strcmp(line, "snap") == 0) {
		if (playlist_is_locked()) {
			snprintf(reply, reply_len, "err locked\n");
		} else {
			rt_pending_post(mbox, PENDING_SNAP);
			snprintf(reply, reply_len, "ok snap\n");
		}

	} else if (strncmp(line, "list", 4) == 0 &&
	           (line[4] == ' ' || line[4] == '\0')) {
		const char *arg = line + 4;
		while (*arg == ' ') arg++;
		if (!*arg) {
			snprintf(reply, reply_len,
			         "err usage: list presets | list playlists [all]\n");
		} else if (strcmp(arg, "presets") == 0) {
			int wrote = playlist_ipc_list_presets(reply, reply_len - 1);
			if (wrote <= 0)
				snprintf(reply, reply_len, "(empty)\n");
		} else if (strncmp(arg, "playlists", 9) == 0 &&
		           (arg[9] == ' ' || arg[9] == '\0')) {
			const char *tail = arg + 9;
			while (*tail == ' ') tail++;
			int show_all = 0;
			if (!*tail) {
				show_all = 0;
			} else if (strcmp(tail, "all") == 0) {
				show_all = 1;
			} else {
				snprintf(reply, reply_len,
				         "err usage: list playlists [all]\n");
				goto list_done;
			}
			int wrote = playlist_render_list_reply(reply, reply_len - 1, show_all);
			if (wrote <= 0)
				snprintf(reply, reply_len, "(empty)\n");
		} else {
			snprintf(reply, reply_len,
			         "err usage: list presets | list playlists [all]\n");
		}
list_done: ;
	/* Toggles */
	} else if (strncmp(line, "shuffle", 7) == 0 &&
	           (line[7] == ' ' || line[7] == '\0')) {
		const char *arg = line + 7;
		while (*arg == ' ') arg++;
		int target = -1; // -1 = toggle (no arg)
		int bad = 0;
		if (*arg) {
			if (strcmp(arg, "on") == 0) target = 1;
			else if (strcmp(arg, "off") == 0) target = 0;
			else bad = 1;
		}
		if (bad) {
			snprintf(reply, reply_len, "err usage: shuffle [on|off]\n");
		} else {
			int cur = playlist_is_shuffled();
			/* Toggle only if state needs to change. For target -1
			 * (no arg) we always toggle. */
			int want = (target == -1) ? !cur : target;
			if (want != cur) {
				rt_pending_post(mbox, PENDING_TOGGLE_SHUFFLE);
			}
			snprintf(reply, reply_len, "ok shuffle %s\n", want ? "on" : "off");
		}

	} else if (strncmp(line, "lock", 4) == 0
	           && (line[4] == ' ' || line[4] == '\0')) {
		const char *arg = line + 4;
		while (*arg == ' ') arg++;
		int target = -1;
		int bad = 0;
		if (*arg) {
			if (strcmp(arg, "on") == 0) target = 1;
			else if (strcmp(arg, "off") == 0) target = 0;
			else bad = 1;
		}
		if (bad) {
			snprintf(reply, reply_len, "err usage: lock [on|off]\n");
		} else {
			int cur = playlist_is_locked();
			int want = (target == -1) ? !cur : target;
			if (want != cur) {
				rt_pending_post(mbox, PENDING_TOGGLE_LOCK);
			}
			snprintf(reply, reply_len, "ok %s\n", want ? "locked" : "unlocked");
		}

	} else if (strncmp(line, "dur", 3) == 0 &&
	           (line[3] == ' ' || line[3] == '\0' ||
	            (strncmp(line + 3, "ation", 5) == 0 &&
	             (line[8] == ' ' || line[8] == '\0')))) {
		const char *arg = line + 3;
		if (strncmp(arg, "ation", 5) == 0) arg += 5;
		while (*arg == ' ') arg++;
		if (*arg) {
			double t = atof(arg);
			if (t > 0) {
				rt_pending_post_duration(mbox, PENDING_SET_DURATION, t);
				snprintf(reply, reply_len, "ok dur %.0fs\n", t);
			} else {
				snprintf(reply, reply_len, "err dur needs a positive number\n");
			}
		} else {
			snprintf(reply, reply_len, "err usage: dur <seconds>\n");
		}

	} else if (strcmp(line, "blacklist") == 0) {
		playlist_ipc_blacklist(mbox, reply, reply_len);

	} else if (strcmp(line, "blacklist-global") == 0) {
		playlist_ipc_blacklist_global(mbox, reply, reply_len);

	} else if (strcmp(line, "undo") == 0) {
		playlist_ipc_undo(mbox, reply, reply_len);

	} else if ((pl_arg = match_pl_verb(line, "load")) != NULL) {
		if (!*pl_arg) {
			snprintf(reply, reply_len, "err usage: playlist load <name>\n");
		} else {
			char resolved[1024];
			if (!resolve_playlist_path(pl_arg, resolved, sizeof(resolved))) {
				snprintf(reply, reply_len, "err invalid playlist name: %s\n", pl_arg);
			} else {
				struct stat st;
				if (stat(resolved, &st) != 0) {
					snprintf(reply, reply_len, "err not found: %s\n", resolved);
				} else if (!S_ISREG(st.st_mode)) {
					snprintf(reply, reply_len, "err not a file: %s\n", resolved);
				} else if (access(resolved, R_OK) != 0) {
					snprintf(reply, reply_len, "err cannot read: %s\n", resolved);
				} else {
					playlist_ipc_load(mbox, resolved);
					/* Reply shows just the basename for a clean overlay flash */
					const char *bn = path_basename(resolved);
					snprintf(reply, reply_len, "ok playlist loaded: %s\n", bn);
				}
			}
		}

	} else if ((pl_arg = match_pl_verb(line, "save")) != NULL) {
		if (!*pl_arg) {
			/* No name given so the caller falls back to the current playlist name */
			playlist_ipc_save(mbox, "", reply, reply_len);

		} else {
			char resolved[1024];
			if (!resolve_playlist_path(pl_arg, resolved, sizeof(resolved))) {
				snprintf(reply, reply_len, "err invalid playlist name: %s\n", pl_arg);
			} else {
				playlist_ipc_save(mbox, resolved, reply, reply_len);

			}
		}

	} else if ((pl_arg = match_pl_verb(line, "new")) != NULL) {
		if (!*pl_arg) {
			snprintf(reply, reply_len, "err usage: playlist new <name> [path]\n");
		} else {
			char name[256] = {0};
			char path[1024] = {0};
			int got = sscanf(pl_arg, "%255s %1023[^\n]", name, path);
			if (got < 1 || !name[0]) {
				snprintf(reply, reply_len, "err usage: playlist new <name> [path]\n");
			} else if (strchr(name, '/')) {
				/* Names can't be paths - they're identifiers */
				snprintf(reply, reply_len, "err name cannot contain '/': %s\n", name);
			} else if (path[0]) {
				/* Path given. Could be a directory of presets, or a playlist
				 * file. Either way it must exist. Resolve bare names against the
				 * playlists dir, accept absolute or relative paths as-is. */
				char resolved_path[1024];
				if (strchr(path, '/')) {
					snprintf(resolved_path, sizeof(resolved_path), "%s", path);
				} else if (!resolve_playlist_path(path, resolved_path,
				                                  sizeof(resolved_path))) {
					resolved_path[0] = '\0';
				}
				struct stat st;
				if (!resolved_path[0] || stat(resolved_path, &st) != 0) {
					snprintf(reply, reply_len, "err not found: %s\n",
					         resolved_path[0] ? resolved_path : path);
				} else {
					playlist_ipc_new(mbox, name, resolved_path);
					snprintf(reply, reply_len, "ok new playlist: %s\n", name);
				}
			} else {
				/* Empty playlist - no path validation needed */
				playlist_ipc_new(mbox, name, NULL);
				snprintf(reply, reply_len, "ok new playlist: %s (empty)\n", name);
			}
		}

	} else if ((pl_arg = match_pl_verb(line, "merge")) != NULL) {
		if (!*pl_arg) {
			snprintf(reply, reply_len, "err usage: playlist merge <name>\n");
		} else {
			char resolved[1024];
			/* Merge accepts file or directory. Bare names resolve to playlists dir. */
			if (strchr(pl_arg, '/'))
				snprintf(resolved, sizeof(resolved), "%s", pl_arg);
			else if (!resolve_playlist_path(pl_arg, resolved, sizeof(resolved)))
				resolved[0] = '\0';

			struct stat st;
			if (!resolved[0] || stat(resolved, &st) != 0) {
				snprintf(reply, reply_len, "err not found: %s\n",
				         resolved[0] ? resolved : pl_arg);
			} else {
				playlist_ipc_merge(mbox, resolved);
				const char *bn = path_basename(resolved);
				snprintf(reply, reply_len, "ok merged: %s\n", bn);
			}
		}

	} else if ((pl_arg = match_pl_verb(line, "add-pin")) != NULL) {
		if (!*pl_arg) {
			snprintf(reply, reply_len, "err usage: playlist add-pin <name>\n");
		} else if (strchr(pl_arg, '/')) {
			snprintf(reply, reply_len, "err name cannot contain '/': %s\n", pl_arg);
		} else {
			char err[256] = {0};
			int ok = playlist_ipc_add(mbox, pl_arg, 1, err, sizeof(err));
			if (ok) {
				snprintf(reply, reply_len, "ok pinned to %s\n", pl_arg);
			} else {
				snprintf(reply, reply_len, "err %s\n",
				         err[0] ? err : "playlist add-pin failed");
			}
		}

	} else if ((pl_arg = match_pl_verb(line, "add")) != NULL) {
		if (!*pl_arg) {
			snprintf(reply, reply_len, "err usage: playlist add <name>\n");
		} else if (strchr(pl_arg, '/')) {
			snprintf(reply, reply_len, "err name cannot contain '/': %s\n", pl_arg);
		} else {
			char err[256] = {0};
			int ok = playlist_ipc_add(mbox, pl_arg, 0, err, sizeof(err));
			if (ok) {
				snprintf(reply, reply_len, "ok added to %s\n", pl_arg);
			} else {
				snprintf(reply, reply_len, "err %s\n",
				         err[0] ? err : "playlist add failed");
			}
		}

	} else if ((pl_arg = match_pl_verb(line, "pin")) != NULL) {
		(void)pl_arg; // pin takes no args. trailing text is ignored
		char err[256] = {0};
		int ok = playlist_ipc_pin(mbox, err, sizeof(err));
		if (ok) {
			snprintf(reply, reply_len, "ok %s\n",
			         err[0] ? err : "pinned");
		} else {
			snprintf(reply, reply_len, "err %s\n",
			         err[0] ? err : "playlist pin failed");
		}

	} else {
		return 0;
	}
	if (!(strncmp(line, "list", 4) == 0 && (line[4] == ' ' || line[4] == '\0')))
		module_emit_reply("playlist", reply);
	return 1;
}

/* rt stashed at registry init so the ipc adapter can post pending
 * actions. Init runs before ipc_start, so it's set before any command. */
static struct rt *g_pl_rt;
static int playlist_mod_init(struct rt *rt) { g_pl_rt = rt; return 1; }

static int playlist_ipc_adapter(struct ipc_command_ctx *c) {
	return playlist_ipc_command(g_pl_rt, c->args, c->reply, c->reply_len)
		? 0 : IPC_CMD_DECLINED;
}

const char *path_basename(const char *path) {
	const char *bn = strrchr(path, '/');
	return bn ? bn + 1 : path;
}

void path_basename_no_ext(const char *path, char *out, size_t outlen) {
	if (!out || outlen == 0) return;
	snprintf(out, outlen, "%s", path_basename(path));
	char *dot = strrchr(out, '.');
	if (dot) *dot = '\0';
}

static char **g_playlist = NULL;
static int g_playlist_count = 0;
static int g_playlist_idx = -1;

/* read on IPC thread (guards and replies), written on render thread */
static _Atomic int g_shuffle = 1;

/* Blacklist state.
 *   g_blacklist is per-playlist, persisted in the #blacklist section.
 *   g_global_blacklist is system-wide, from <config_dir>/playlists/global.blacklist.
 *   Single-level undo across both lanes (g_blacklisted_path and g_last_blacklist_lane). */
enum blacklist_lane { BLACKLIST_LANE_NONE = 0, BLACKLIST_LANE_PLAYLIST, BLACKLIST_LANE_GLOBAL };

static char **g_blacklist = NULL;
static int g_blacklist_count = 0;
static char **g_global_blacklist = NULL;
static int g_global_blacklist_count = 0;

static char *g_blacklisted_path = NULL;
static int g_blacklisted_idx = -1;
static enum blacklist_lane g_last_blacklist_lane = BLACKLIST_LANE_NONE;

static char g_playlist_name[256] = "default";

/* Verbatim "#name ..." header line from the loaded file, if it
 * had one. The parser otherwise treats it as a comment. We keep the raw text
 * so save round-trips it instead of clobbering it. Empty = none captured. */
static char g_playlist_header[1024] = "";

/* Shuffle prev-history. */
#define NAV_HISTORY_CAP 64
static char **g_nav_history = NULL;
static int g_nav_history_count = 0;

/* Canonical file path the loaded playlist came from (empty = no source). */
static char g_playlist_src_path[1024] = "";

/* First line of daemon-generated source-mirror playlists. Files lacking
 * this are user-owned and never touched by regen. Sentinel-loaded playlists
 * suppress autosave and reject playlist-save. */
#define SOURCE_PLAYLIST_SENTINEL \
	"# auto-generated; do not edit, edits will be lost on next boot"

/* In-file marker separating active entries from per-playlist blacklist. */
#define PLAYLIST_BLACKLIST_MARKER "#blacklist"

/* True if `path` appears in the given path array. Linear scan. Arrays are
 * small (hundreds at most) so we don't bother with a hash set. */
static int path_in_set(const char *path, char *const *set, int set_count) {
	for (int i = 0; i < set_count; i++)
		if (strcmp(set[i], path) == 0) return 1;
	return 0;
}

/* Append a strdup'd copy of `path` to a heap path array. On either allocation
 * failing the array is left exactly as it was, so a caller that ignores the
 * result still holds a consistent array. Returns 0 on allocation failure. */
static int path_set_append(char ***set, int *count, const char *path) {
	char *copy = strdup(path);
	if (!copy) return 0;

	char **grown = realloc(*set, (size_t)(*count + 1) * sizeof(char *));
	if (!grown) {
		free(copy);
		return 0;
	}

	*set = grown;
	(*set)[(*count)++] = copy;
	return 1;
}

/* Same contract, cap-doubling growth for arrays built in a scan loop. */
static int path_vec_append(char ***arr, int *count, int *cap, const char *path) {
	char *copy = strdup(path);
	if (!copy) return 0;

	if (*count >= *cap) {
		int newcap = *cap ? *cap * 2 : 16;
		char **grown = realloc(*arr, (size_t)newcap * sizeof(char *));
		if (!grown) {
			free(copy);
			return 0;
		}
		*arr = grown;
		*cap = newcap;
	}

	(*arr)[(*count)++] = copy;
	return 1;
}

/* Append to the active set. */
static int playlist_append(const char *path) {
	return path_set_append(&g_playlist, &g_playlist_count, path);
}

/* Remove the first array entry matching `path` (linear scan). Frees the
 * removed string. No-op if not found. */
static void path_set_remove(char ***set, int *count, const char *path) {
	for (int i = 0; i < *count; i++) {
		if (strcmp((*set)[i], path) == 0) {
			free((*set)[i]);
			for (int j = i; j < *count - 1; j++)
				(*set)[j] = (*set)[j + 1];
			(*count)--;
			return;
		}
	}
}

/* Free all entries in a path array and the array itself. Sets out args to
 * NULL and 0 so the caller's locals are safe to use again. */
static void path_set_free(char ***set, int *count) {
	for (int i = 0; i < *count; i++) free((*set)[i]);
	free(*set);
	*set = NULL;
	*count = 0;
}

/* Parse a playlist file into active entries and blacklist entries.
 * Returns 0 if file doesn't exist. */
static int parse_playlist_file(const char *path,
                               char ***active, int *n_active,
                               char ***blist, int *n_blist) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int in_blacklist = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		int ll = (int)strlen(line);
		while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
			line[--ll] = '\0';
		if (ll == 0) continue;
		{
			size_t mlen = strlen(PLAYLIST_BLACKLIST_MARKER);
			if (strncmp(line, PLAYLIST_BLACKLIST_MARKER, mlen) == 0
			    && (line[mlen] == '\0' || line[mlen] == ' ' || line[mlen] == '\t'))
			{
				in_blacklist = 1;
				continue;
			}
		}
		if (line[0] == '#') continue;
		if (in_blacklist)
			path_set_append(blist, n_blist, line);
		else
			path_set_append(active, n_active, line);
	}
	fclose(f);
	return 1;
}

/* Write a playlist file with the standard header and active section,
 * plus the #blacklist marker and blacklist section if non-empty.
 * Inverse of parse_playlist_file. */
static int write_playlist_file(const char *path,
                               char *const *active, int n_active,
                               char *const *blist, int n_blist) {
	FILE *fw = fopen(path, "w");
	if (!fw) return 0;
	fprintf(fw, "# " APP_ID " playlist\n");
	for (int i = 0; i < n_active; i++)
		fprintf(fw, "%s\n", active[i]);
	if (n_blist > 0) {
		fprintf(fw, "%s\n", PLAYLIST_BLACKLIST_MARKER);
		for (int i = 0; i < n_blist; i++)
			fprintf(fw, "%s\n", blist[i]);
	}
	fclose(fw);
	return 1;
}

static void global_blacklist_path(char *out, int outlen) {
	snprintf(out, outlen, "%s/global.blacklist", app_paths_playlists_dir());
}

void playlist_path_for_name(const char *name, char *out, size_t outlen) {
	size_t nl = strlen(name);
	const char *suffix = (nl >= 4 && strcmp(name + nl - 4, ".lst") == 0)
					   ? "" : ".lst";
	snprintf(out, outlen, "%s/%s%s",
	         app_paths_playlists_dir(), name, suffix);
}

/* Load global.blacklist into g_global_blacklist. Missing file = empty. */
static void load_global_blacklist(void) {
	/* Reset existing state. Reload should reflect current file state. */
	for (int i = 0; i < g_global_blacklist_count; i++)
		free(g_global_blacklist[i]);
	free(g_global_blacklist);
	g_global_blacklist = NULL;
	g_global_blacklist_count = 0;

	char path[1024];
	global_blacklist_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return; // absent = empty global blacklist

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		int n = (int)strlen(line);
		while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
			line[--n] = '\0';
		if (!n || line[0] == '#') continue;
		path_set_append(&g_global_blacklist, &g_global_blacklist_count, line);
	}
	fclose(f);
	DBG("[playlist] global blacklist: %d entries", g_global_blacklist_count);
}

/* Write g_global_blacklist to disk. Atomic: tempfile + rename. */
static int save_global_blacklist(void) {
	char path[1024];
	global_blacklist_path(path, sizeof(path));
	char tmp[1024 + 8]; // room for the ".tmp" suffix
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		LOG("[playlist] cannot write global blacklist: %s", strerror(errno));
		return 0;
	}
	fprintf(f, "# " APP_ID " global blacklist\n");
	fprintf(f, "# Presets listed here are excluded from every playlist load,\n");
	fprintf(f, "# merge, and add. Managed by `blacklist-global` / `undo`.\n");
	for (int i = 0; i < g_global_blacklist_count; i++)
		fprintf(f, "%s\n", g_global_blacklist[i]);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		LOG("[playlist] cannot rename global blacklist: %s", strerror(errno));
		return 0;
	}
	return 1;
}

/* True if the currently-loaded playlist file began with SOURCE_PLAYLIST_SENTINEL.
 * Set by playlist_load_from (when the sentinel is on the first line) and
 * cleared on PL_SAVE, PL_NEW, or PL_RESCAN. Gates autosave_current() and
 * the ipc_playlist_save same-name guard. */
static bool g_playlist_is_source = false;

/* Wall-clock instant the current preset was loaded. Used by the
 * auto-advance decision (via playlist_preset_age_seconds). */
static struct timespec g_timer_start;

/* Auto-advance intervals, 0 disables. Seeded from playlist's own
 * `duration` slice at apply, overridable live via the `dur` IPC verb.
 * Written on main thread at bringup and reload, read on the IPC thread. */
static _Atomic double g_auto_advance_seconds = 0.0;

/* Lock - when locked, auto-advance, next, and prev are disabled. Read on
 * IPC thread (lock guard), written on render thread */
static _Atomic int g_locked = 0;

/* IPC read snapshot storage. Function definitions live further down by the
 * accessors. Storage lives here with the other module state. The render
 * thread is the sole writer of everything above. This snapshot is the only
 * thing the IPC thread ever touches. */
static pthread_mutex_t g_snap_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_snap_dirty = 1; // render-thread only
static struct playlist_view g_snap_view; // published fixed view
static char **g_snap_entries = NULL; // published deep copy of g_playlist
static int g_snap_entries_count = 0;

/* Deferred preset overlay - fires after the visualizer finishes its soft cut. */
static struct timespec g_transition_time;
static char g_transition_text[512];
static int g_transition_pending = 0;

void playlist_scan_directory(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

		unsigned char dt = ent->d_type;
		if (dt == DT_UNKNOWN) {
			struct stat st;
			dt = (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				? DT_DIR : DT_REG;
		}

		if (dt == DT_DIR) {
			playlist_scan_directory(path);
		} else if (preset_ext_match(ent->d_name)) {
			if (path_in_set(path, g_global_blacklist, g_global_blacklist_count))
				continue;
			playlist_append(path);
		}
	}
	closedir(d);
}

/* Collect files recursively into a caller-supplied vector, leaving the
 * active set alone. Unlike `playlist_scan_directory` this does NOT apply the
 * global blacklist: it generates the on-disk source mirrors, which stay
 * pristine so that un-blacklisting re-enables a file at the next load without
 * a regen. */
static void scan_preset_files_into(const char *dir,
                                 char ***arr, int *count, int *cap) {
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

		unsigned char dt = ent->d_type;
		if (dt == DT_UNKNOWN) {
			struct stat st;
			dt = (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				? DT_DIR : DT_REG;
		}

		if (dt == DT_DIR) {
			scan_preset_files_into(path, arr, count, cap);
		} else if (preset_ext_match(ent->d_name)) {
			path_vec_append(arr, count, cap, path);
		}
	}
	closedir(d);
}

static void scan_loose_preset_files_into(const char *dir,
                                 char ***arr, int *count, int *cap) {
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

		unsigned char dt = ent->d_type;
		if (dt == DT_UNKNOWN) {
			struct stat st;
			dt = (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				? DT_DIR : DT_REG;
		}

		if (dt != DT_DIR && preset_ext_match(ent->d_name))
			path_vec_append(arr, count, cap, path);
	}
	closedir(d);
}

static int strcmp_indirect(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int playlist_file_has_sentinel(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char first[512];
	int has = 0;
	if (fgets(first, sizeof(first), f)) {
		if (strncmp(first, SOURCE_PLAYLIST_SENTINEL,
		            strlen(SOURCE_PLAYLIST_SENTINEL)) == 0)
			has = 1;
	}
	fclose(f);
	return has;
}

static void write_source_mirror(const char *target, const char *name,
                                char **paths, int pcount, int target_existed,
                                char **new_names, int *new_count) {
	FILE *out = fopen(target, "w");
	if (!out) {
		LOG("[playlists] cannot write %s: %s", target, strerror(errno));
		return;
	}
	fprintf(out, "%s\n", SOURCE_PLAYLIST_SENTINEL);
	for (int i = 0; i < pcount; i++)
		fprintf(out, "%s\n", paths[i]);
	fclose(out);
	DBG("[playlists] %s.lst: %d presets", name, pcount);
	if (!target_existed && *new_count < 64) {
		char *copy = strdup(name);
		if (copy) new_names[(*new_count)++] = copy;
	}
}

/* Generate per-subdirectory source-mirror .lst files for one root. */
static void regen_mirrors_for_root(const char *root, const char *playlists_dir,
                                   char **new_names, int *new_count) {
	DIR *d = opendir(root);
	if (!d) {
		DBG("[playlists] cannot enumerate %s: %s",
		    root, strerror(errno));
		return;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;
		char subdir[1024];
		snprintf(subdir, sizeof(subdir), "%s/%s",
		         root, ent->d_name);
		struct stat st;
		if (stat(subdir, &st) != 0) continue;
		if (!S_ISDIR(st.st_mode)) continue;

		char target[2048]; // dir + '/' + entry + ".lst"
		snprintf(target, sizeof(target),
		         "%s/%s.lst", playlists_dir, ent->d_name);

		int target_existed = (access(target, F_OK) == 0);
		if (target_existed && !playlist_file_has_sentinel(target)) {
			DBG("[playlists] %s.lst is user-owned, skipping regen",
			    ent->d_name);
			continue;
		}

		char **paths = NULL;
		int pcount = 0, pcap = 0;
		scan_preset_files_into(subdir, &paths, &pcount, &pcap);
		if (pcount == 0) {
			DBG("[playlists] %s: no preset files, skipping", ent->d_name);
			free(paths);
			continue;
		}
		qsort(paths, pcount, sizeof(char *), strcmp_indirect);

		write_source_mirror(target, ent->d_name, paths, pcount,
		                    target_existed, new_names, new_count);
		for (int i = 0; i < pcount; i++) free(paths[i]);
		free(paths);
	}
	closedir(d);

	char target[2048];
	snprintf(target, sizeof(target), "%s/%s.lst",
	         playlists_dir, path_basename(root));
	int target_existed = (access(target, F_OK) == 0);
	if (target_existed && !playlist_file_has_sentinel(target)) {
		DBG("[playlists] %s.lst is user-owned, skipping regen",
		    path_basename(root));
		return;
	}

	char **paths = NULL;
	int pcount = 0, pcap = 0;
	scan_loose_preset_files_into(root, &paths, &pcount, &pcap);
	if (pcount == 0) {
		DBG("[playlists] %s: no loose preset files, skipping",
		    path_basename(root));
		free(paths);
		return;
	}
	qsort(paths, pcount, sizeof(char *), strcmp_indirect);

	write_source_mirror(target, path_basename(root), paths, pcount,
	                    target_existed, new_names, new_count);
	for (int i = 0; i < pcount; i++) free(paths[i]);
	free(paths);
}

void playlist_rebuild_source_mirrors(void) {
	char playlists_dir[1024];
	snprintf(playlists_dir, sizeof(playlists_dir),
	         "%s", app_paths_playlists_dir());
	mkdir(playlists_dir, 0755); // harmless if exists

	char *new_names[64];
	int new_count = 0;

	for (int ri = 0; ri < presets_dir_count(); ri++)
		regen_mirrors_for_root(presets_dir_at(ri), playlists_dir,
		                       new_names, &new_count);

	if (new_count > 0) {
		char list[768];
		int off = 0;
		for (int i = 0; i < new_count; i++) {
			off += snprintf(list + off, sizeof(list) - off,
			                "%s%s", i ? ", " : "", new_names[i]);
			free(new_names[i]);
			if (off >= (int)sizeof(list) - 1) break;
		}
		struct overlay_spec dw;
		overlay_spec_init(&dw);
		snprintf(dw.text, sizeof(dw.text),
		         "new preset sources:\n%s", list);
		dw.duration_ms = 8000;
		overlay_show(&dw);
		DBG("[playlists] %d new source(s): %s", new_count, list);
	}
}

/* Push a copy of `path` as the newest history entry, dropping
 * the oldest once NAV_HISTORY_CAP is reached. No-op on NULL or OOM. */
static void nav_history_push(const char *path) {
	if (!path) return;
	if (!g_nav_history) {
		g_nav_history = calloc(NAV_HISTORY_CAP, sizeof(char *));
		if (!g_nav_history) return;
		g_nav_history_count = 0;
	}
	if (g_nav_history_count == NAV_HISTORY_CAP) {
		free(g_nav_history[0]);
		memmove(&g_nav_history[0], &g_nav_history[1],
		        (NAV_HISTORY_CAP - 1) * sizeof(char *));
		g_nav_history_count--;
	}
	char *copy = strdup(path);
	if (!copy) return;
	g_nav_history[g_nav_history_count++] = copy;
}

/* Pop the newest entry, transferring ownership to the caller
 * (who frees it). Returns NULL when the history is empty. */
static char *nav_history_pop(void) {
	if (g_nav_history_count == 0) return NULL;
	char *top = g_nav_history[--g_nav_history_count];
	g_nav_history[g_nav_history_count] = NULL;
	return top;
}

static void nav_history_clear(void) {
	for (int i = 0; i < g_nav_history_count; i++)
		free(g_nav_history[i]);
	free(g_nav_history);
	g_nav_history = NULL;
	g_nav_history_count = 0;
}

/* Free all playlist entries including the per-playlist blacklist set.
 * Does NOT touch g_global_blacklist. */
static void playlist_clear(void) {
	for (int i = 0; i < g_playlist_count; i++)
		free(g_playlist[i]);
	free(g_playlist);
	g_playlist = NULL;
	g_playlist_count = 0;
	g_playlist_idx = -1;

	for (int i = 0; i < g_blacklist_count; i++)
		free(g_blacklist[i]);
	free(g_blacklist);
	g_blacklist = NULL;
	g_blacklist_count = 0;

	nav_history_clear();
}

/* Remove preset at index, return the removed path (caller frees) */
static char *playlist_remove_at(int idx) {
	if (idx < 0 || idx >= g_playlist_count) return NULL;
	char *removed = g_playlist[idx];
	for (int i = idx; i < g_playlist_count - 1; i++)
		g_playlist[i] = g_playlist[i + 1];
	g_playlist_count--;

	if (g_playlist_idx >= g_playlist_count)
		g_playlist_idx = 0;
	else if (g_playlist_idx > idx)
		g_playlist_idx--;
	return removed;
}

/* Takes ownership of `path`, and frees it if the array cannot grow. */
static void playlist_insert_at(int idx, char *path) {
	if (idx < 0) idx = 0;
	if (idx > g_playlist_count) idx = g_playlist_count;

	char **grown = realloc(g_playlist,
	                       (size_t)(g_playlist_count + 1) * sizeof(char *));
	if (!grown) {
		free(path);
		return;
	}
	g_playlist = grown;

	for (int i = g_playlist_count; i > idx; i--)
		g_playlist[i] = g_playlist[i - 1];
	g_playlist[idx] = path;
	g_playlist_count++;
	if (g_playlist_idx >= idx)
		g_playlist_idx++;
}

static int playlist_contains(const char *path) {
	for (int i = 0; i < g_playlist_count; i++)
		if (strcmp(g_playlist[i], path) == 0) return 1;
	return 0;
}

int playlist_save_file(const char *filepath) {
	FILE *f = fopen(filepath, "w");
	if (!f) return 0;
	if (g_playlist_header[0])
		fprintf(f, "%s\n", g_playlist_header);
	else
		fprintf(f, "# " APP_ID " playlist\n");
	for (int i = 0; i < g_playlist_count; i++)
		fprintf(f, "%s\n", g_playlist[i]);
	if (g_blacklist_count > 0) {
		fprintf(f, "%s\n", PLAYLIST_BLACKLIST_MARKER);
		for (int i = 0; i < g_blacklist_count; i++)
			fprintf(f, "%s\n", g_blacklist[i]);
	}
	fclose(f);
	return 1;
}

/* Derive the .autosave sibling path from a canonical playlist path. Replaces a
 * trailing ".lst" with ".autosave". If the source lacks the .lst extension we
 * just append ".autosave". Always NULL-terminates. Returns 1 on success, 0 if
 * out buffer is too small. */
static int autosave_path_from(const char *source, char *out, int outlen) {
	if (!source || !source[0] || !out || outlen <= 0) return 0;
	int slen = (int)strlen(source);
	const char *dot = strrchr(source, '.');
	if (dot && strcmp(dot, ".lst") == 0) {
		int base_len = (int)(dot - source);
		if (base_len + 10 >= outlen) return 0; // ".autosave" + NULL
		memcpy(out, source, base_len);
		memcpy(out + base_len, ".autosave", 10); // includes + NULL
		return 1;
	}
	if (slen + 10 >= outlen) return 0;
	memcpy(out, source, slen);
	memcpy(out + slen, ".autosave", 10);
	return 1;
}

static void autosave_current(void) {
	if (!g_playlist_src_path[0]) return; // no canonical source
	if (g_playlist_is_source) return; // source mirror - read-only
	char ap[1024];
	if (!autosave_path_from(g_playlist_src_path, ap, sizeof(ap))) return;
	if (!playlist_save_file(ap)) {
		LOG("[autosave] write FAILED: %s: %s", ap, strerror(errno));
	} else {
		DBG("[autosave] wrote %s", ap);
	}
}

int playlist_load_file(const char *filepath) {
	FILE *f = fopen(filepath, "r");
	if (!f) return 0;

	playlist_clear();
	g_playlist_is_source = false;
	g_playlist_header[0] = '\0';

	int first_line = 1;
	int in_blacklist = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		int len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = '\0';

		if (first_line) {
			first_line = 0;
			if (strncmp(line, SOURCE_PLAYLIST_SENTINEL,
			            strlen(SOURCE_PLAYLIST_SENTINEL)) == 0) {
				g_playlist_is_source = true;
				continue;
			}
			/* Keep leading "#name" header so save preserves it. */
			if (strncmp(line, "#name", 5) == 0)
				snprintf(g_playlist_header, sizeof(g_playlist_header), "%s", line);
		}
		if (!len) continue;

		/* Section toggle, bare "#blacklist" line moves us from active to
		 * blacklist parsing. The match is strict: leading whitespace is
		 * not allowed, trailing whitespace is OK. */
		{
			size_t mlen = strlen(PLAYLIST_BLACKLIST_MARKER);
			if (strncmp(line, PLAYLIST_BLACKLIST_MARKER, mlen) == 0
			    && (line[mlen] == '\0' || line[mlen] == ' ' || line[mlen] == '\t'))
			{
				in_blacklist = 1;
				continue;
			}
		}

		if (line[0] == '#') continue;
		if (access(line, R_OK) != 0) continue;

		/* Global blacklist filter. Applies to BOTH blacklists. A globally-
		 * blacklisted preset shouldn't appear in g_playlist or in
		 * g_blacklist. */
		if (path_in_set(line, g_global_blacklist, g_global_blacklist_count))
			continue;

		if (in_blacklist)
			path_set_append(&g_blacklist, &g_blacklist_count, line);
		else
			playlist_append(line);
	}
	fclose(f);

	if (g_playlist_count > 0) {
		if (g_shuffle) playlist_shuffle();
		else g_playlist_idx = 0;
	}
	return 1;
}

int playlist_merge_file(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return 0;

	if (S_ISDIR(st.st_mode)) {
		char **scanned = NULL;
		int nscanned = 0, capscanned = 0;
		scan_preset_files_into(path, &scanned, &nscanned, &capscanned);
		for (int i = 0; i < nscanned; i++) {
			const char *p = scanned[i];
			if (!playlist_contains(p)
			    && !path_in_set(p, g_blacklist, g_blacklist_count)
			    && !path_in_set(p, g_global_blacklist, g_global_blacklist_count))
			{
				playlist_append(p);
			}
			free(scanned[i]);
		}
		free(scanned);
		return 1;
	} else if (S_ISREG(st.st_mode)) {
		FILE *f = fopen(path, "r");
		if (!f) return 0;
		int src_in_blacklist = 0;
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			int len = (int)strlen(line);
			while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
				line[--len] = '\0';
			if (!len) continue;
			/* Source file's own blacklist marker - everything below is the
			 * source file's blacklist and must NOT enter our active set. */
			{
				size_t mlen = strlen(PLAYLIST_BLACKLIST_MARKER);
				if (strncmp(line, PLAYLIST_BLACKLIST_MARKER, mlen) == 0
				    && (line[mlen] == '\0' || line[mlen] == ' ' || line[mlen] == '\t'))
				{
					src_in_blacklist = 1;
					continue;
				}
			}
			if (src_in_blacklist) continue;
			if (line[0] == '#') continue;
			if (access(line, R_OK) != 0) continue;
			if (playlist_contains(line)) continue;
			if (path_in_set(line, g_blacklist, g_blacklist_count)) continue;
			if (path_in_set(line, g_global_blacklist,
			                g_global_blacklist_count)) continue;
			playlist_append(line);
		}
		fclose(f);
		return 1;
	}
	return 0;
}

void playlist_load_current(bool smooth) {
	if (g_playlist_count == 0 || g_playlist_idx < 0) return;
	const char *path = g_playlist[g_playlist_idx % g_playlist_count];

	const char *basename = path_basename(path);

	snprintf(g_transition_text, sizeof(g_transition_text),
	         "[%s]  %d / %d\n%s",
	         g_playlist_name,
	         g_playlist_idx + 1,
	         g_playlist_count, basename);

	struct visualizer *vis = visualizer_ensure_for(path);
	if (!vis) {
		DBG("[playlist] no engine for %s", path);
		return;
	}
	if (!vis->load_preset(vis, path, smooth))
		DBG("[playlist] load rejected: %s", path);
	clock_gettime(CLOCK_MONOTONIC, &g_timer_start);
	DBG("[playlist] %d/%d: %s%s",
	        g_playlist_idx + 1, g_playlist_count, path,
	        smooth ? "" : " (snap)");

	/* Queue the preset slot update. With a soft cut we wait for the blend to
	 * finish so the name appears with the new visuals. With a hard snap the
	 * cut is instant so we can fire the slot immediately. */
	if (smooth) {
		clock_gettime(CLOCK_MONOTONIC, &g_transition_time);
		g_transition_pending = 1;
	} else {
		info_preset(g_transition_text);
		g_transition_pending = 0;
		/* Snap path: no soft cut window, so g_transition_pending
		 * never rises and the pacer's per-frame edge handler wouldn't
		 * see this preset change. Report the cut explicitly. */
		const struct frame_pacer *pacer = module_active_pacer();
		if (pacer && pacer->preset_cut) pacer->preset_cut();
	}
}

/* Playlist access log. <config_dir>/playlists/.access_log. One line per
 * load, append-only with dedup. Used by `list playlists` to sort by
 * recency. Logged only on user-initiated load. */
#define PLAYLIST_LOG_MAX 256 // in-memory cap for list rendering
#define PLAYLIST_LIST_DEFAULT 7 // default `list playlists` row count

struct pl_log_entry {
	char name[256];
	long ts;
};

static void playlist_access_log_path(char *out, size_t outlen) {
	snprintf(out, outlen, "%s/.access_log", app_paths_playlists_dir());
}

void playlist_access_log_append(const char *name) {
	if (!name || !*name) return;
	char path[1024];
	playlist_access_log_path(path, sizeof(path));

	/* Adjacent dedup. Read the last line, compare. We only need the
	 * tail of the file. Files smaller than 512 bytes get read entirely. */
	FILE *f = fopen(path, "r");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		if (sz > 0) {
			long start = (sz > 512) ? sz - 512 : 0;
			fseek(f, start, SEEK_SET);
			char tail[513];
			size_t n = fread(tail, 1, sizeof(tail) - 1, f);
			tail[n] = '\0';
			/* Strip trailing newline so strrchr finds the line before it. */
			while (n > 0 && tail[n - 1] == '\n') tail[--n] = '\0';
			char *line_start = strrchr(tail, '\n');
			line_start = line_start ? line_start + 1 : tail;
			/* Format: "<ts> <name>". name follows the first space. */
			char *sp = strchr(line_start, ' ');
			if (sp && strcmp(sp + 1, name) == 0) {
				fclose(f);
				return; // dedup hit
			}
		}
		fclose(f);
	}

	/* Ensure parent dir exists. Harmless if it already does. */
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", app_paths_playlists_dir());
	mkdir(dir, 0755);

	f = fopen(path, "a");
	if (!f) return;
	fprintf(f, "%ld %s\n", (long)time(NULL), name);
	fclose(f);
}

/* Load the access log, collapsing repeated names so each playlist
 * appears once with its most-recent timestamp. Returns the entry
 * count. Out array is filled up to max_entries. */
static int playlist_access_log_load(struct pl_log_entry *out, int max_entries) {
	char path[1024];
	playlist_access_log_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return 0;

	int n = 0;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		long ts;
		char name[256];
		if (sscanf(line, "%ld %255s", &ts, name) != 2) continue;
		int found = -1;
		for (int i = 0; i < n; i++) {
			if (strcmp(out[i].name, name) == 0) { found = i; break; }
		}
		if (found >= 0) {
			if (ts > out[found].ts) out[found].ts = ts;
		} else if (n < max_entries) {
			snprintf(out[n].name, sizeof(out[n].name), "%s", name);
			out[n].ts = ts;
			n++;
		}
	}
	fclose(f);
	return n;
}

static int pl_log_cmp_desc(const void *a, const void *b) {
	const struct pl_log_entry *ea = a;
	const struct pl_log_entry *eb = b;
	if (ea->ts < eb->ts) return 1;
	if (ea->ts > eb->ts) return -1;
	return 0;
}

/* Format a relative age (now - past, in seconds) into a short string:
 * "12s", "5m", "3h", "2d", "4w". Caller supplies the buffer. */
static void format_relative_time(long age_sec, char *out, size_t outlen) {
	if (age_sec < 0) age_sec = 0;
	if (age_sec < 60) snprintf(out, outlen, "%lds", age_sec);
	else if (age_sec < 3600) snprintf(out, outlen, "%ldm", age_sec / 60);
	else if (age_sec < 86400) snprintf(out, outlen, "%ldh", age_sec / 3600);
	else if (age_sec < 7 * 86400) snprintf(out, outlen, "%ldd", age_sec / 86400);
	else snprintf(out, outlen, "%ldw", age_sec / (7 * 86400));
}

static int strcasecmp_qsort(const void *a, const void *b) {
	return strcasecmp(*(const char **)a, *(const char **)b);
}

/* Collect the .lst names in the playlists dir that the access log has never
 * seen, sorted A-Z. Caller frees with `path_set_free`. */
static char **playlist_scan_unloaded(const struct pl_log_entry *log,
                                     int log_count, int *out_n)
{
	char **names = NULL;
	int n = 0, cap = 0;
	*out_n = 0;

	DIR *d = opendir(app_paths_playlists_dir());
	if (!d) return NULL;

	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue; // . .. .access_log
		const char *dot = strrchr(ent->d_name, '.');
		if (!dot || strcmp(dot, ".lst") != 0) continue;

		char base[256];
		snprintf(base, sizeof(base), "%.*s",
		         (int)(dot - ent->d_name), ent->d_name);

		int in_log = 0;
		for (int i = 0; i < log_count; i++) {
			if (strcmp(log[i].name, base) == 0) { in_log = 1; break; }
		}
		if (in_log) continue;

		path_vec_append(&names, &n, &cap, base);
	}
	closedir(d);

	qsort(names, n, sizeof(char *), strcasecmp_qsort);
	*out_n = n;
	return names;
}

/* Emit rows until the row cap or the buffer runs out. */
static void playlist_emit_log_rows(char *buf, int buflen, int *off,
                                   const struct pl_log_entry *log, int rows,
                                   long now)
{
	for (int i = 0; i < rows && *off < buflen - 2; i++) {
		char tbuf[16];
		format_relative_time(now - log[i].ts, tbuf, sizeof(tbuf));
		int marker = (strcmp(log[i].name, g_playlist_name) == 0);
		*off += snprintf(buf + *off, buflen - *off, "%s%-24s %s\n",
		                 marker ? "* " : "  ", log[i].name, tbuf);
	}
}

static void playlist_emit_unloaded_rows(char *buf, int buflen, int *off,
                                        char *const *names, int rows)
{
	for (int i = 0; i < rows && *off < buflen - 2; i++) {
		int marker = (strcmp(names[i], g_playlist_name) == 0);
		*off += snprintf(buf + *off, buflen - *off, "%s%-24s -\n",
		                 marker ? "* " : "  ", names[i]);
	}
}

/* Default mode caps total rows at PLAYLIST_LIST_DEFAULT, all mode is unbounded.
 * The log fills the cap first by recency, then never-loaded entries backfill
 * whatever is left, A-Z. */
int playlist_render_list_reply(char *buf, int buflen, int show_all) {
	struct pl_log_entry log_entries[PLAYLIST_LOG_MAX];
	int log_count = playlist_access_log_load(log_entries, PLAYLIST_LOG_MAX);
	qsort(log_entries, log_count, sizeof(struct pl_log_entry), pl_log_cmp_desc);

	int row_limit = show_all ? INT_MAX : PLAYLIST_LIST_DEFAULT;
	int log_rows = log_count < row_limit ? log_count : row_limit;

	/* Scanned up front even when the log already fills the cap: the footer
	 * count needs to know how many never-loaded entries were left out. */
	int unloaded_n = 0;
	char **unloaded = playlist_scan_unloaded(log_entries, log_count,
	                                         &unloaded_n);

	int off = 0;

	/* On a fresh install the log is empty and every row is an alphabetical
	 * fallback, so the header would be a lie. */
	if (!show_all && log_rows > 0 && off < buflen - 2)
		off += snprintf(buf + off, buflen - off, "most recent:\n");

	playlist_emit_log_rows(buf, buflen, &off, log_entries, log_rows,
	                       time(NULL));

	/* Row counts below are what the caps allow, not what the buffer took. */
	int rows_remaining = row_limit - log_rows;
	int unloaded_rows = 0;
	if (rows_remaining > 0) {
		unloaded_rows = unloaded_n < rows_remaining
		                ? unloaded_n : rows_remaining;
		playlist_emit_unloaded_rows(buf, buflen, &off, unloaded,
		                            unloaded_rows);
	}

	if (!show_all) {
		int hidden = (log_count - log_rows)
		             + (unloaded_n - unloaded_rows);
		if (hidden > 0 && off < buflen - 2)
			off += snprintf(buf + off, buflen - off,
			                "  (%d more...)\n", hidden);
	}

	path_set_free(&unloaded, &unloaded_n);

	/* snprintf returns the length it WOULD have written, so a truncated line
	 * can push `off` past buflen even though every write stayed in bounds.
	 * Clamp so the caller never over-reads when it sends the reply. */
	if (off > buflen - 1) off = buflen - 1;
	return off;
}

void playlist_config_apply(void) {
	playlist_set_shuffled(s_cfg.shuffle);
	playlist_set_auto_advance(s_cfg.duration);
}

double playlist_configured_duration(void) { return s_cfg.duration; }

void playlist_init(void) {
	playlist_config_apply(); // seed shuffle from playlist's slice
	g_locked = 0;
	load_global_blacklist();
	clock_gettime(CLOCK_MONOTONIC, &g_timer_start);
}

void playlist_bringup(void) {
	playlist_init();

	/* Regenerate source-mirror playlists. One .lst per immediate subdir of
	 * presets_dir, sentinel-marked. User-owned names with the same basename
	 * are detected (no-sentinel first line) and left alone. */
	playlist_rebuild_source_mirrors();

	/* Auto-load configured playlist (autosave-prefer). Fall back to a
	 * directory scan if nothing was configured or load failed. */
	int from_autosave = 0;
	int loaded = playlist_auto_load(&from_autosave);
	if (loaded) {
		DBG("[playlist] auto-loaded '%s'%s (%d presets)",
		    playlist_name(), from_autosave ? " from autosave" : "",
		    playlist_count());
		if (from_autosave) {
			struct overlay_spec d;
			overlay_spec_init(&d);
			snprintf(d.text, sizeof(d.text),
			    "%s: restored from autosave", playlist_name());
			d.duration_ms = 4000;
			overlay_show(&d);
		}
	} else {
		DBG("[visualizer] scanning %s", presets_dir());
		playlist_scan_presets_dir_fallback();
	}
	DBG("[visualizer] %d presets in playlist", playlist_count());

	/* Select the current preset. The actual load into the engine is
	 * deferred to playlist_load_initial (engine isn't up yet). */
	if (playlist_count() > 0) {
		srand((unsigned)time(NULL));
		if (playlist_is_shuffled()) playlist_shuffle();
		else playlist_set_idx(0);
	} else {
		fprintf(stderr, "[visualizer] WARNING: no presets found\n");
	}
}

void playlist_load_initial(void) {
	if (playlist_count() > 0) playlist_load_current(true);
}

void playlist_shutdown(void) {
	playlist_clear();
	path_set_free(&g_global_blacklist, &g_global_blacklist_count);
	if (g_blacklisted_path) { free(g_blacklisted_path); g_blacklisted_path = NULL; }
	pthread_mutex_lock(&g_snap_lock);
	for (int i = 0; i < g_snap_entries_count; i++) free(g_snap_entries[i]);
	free(g_snap_entries);
	g_snap_entries = NULL;
	g_snap_entries_count = 0;
	pthread_mutex_unlock(&g_snap_lock);
}

void playlist_reload(void) {
	/* g_shuffle is sticky across reload. A user toggle via IPC may differ
	 * from cfg. Only the global blacklist + source mirrors get re-read. */
	load_global_blacklist();
	playlist_rebuild_source_mirrors();
}

int playlist_count(void) { return g_playlist_count; }
int playlist_idx(void) { return g_playlist_idx; }
const char *playlist_at(int idx) {
	if (idx < 0 || idx >= g_playlist_count) return NULL;
	return g_playlist[idx];
}
const char *playlist_current_path(void) {
	if (g_playlist_count == 0 || g_playlist_idx < 0) return NULL;
	return g_playlist[g_playlist_idx % g_playlist_count];
}
const char *playlist_name(void) { return g_playlist_name; }
const char *playlist_src_path(void) { return g_playlist_src_path; }
int playlist_is_source(void) { return g_playlist_is_source ? 1 : 0; }

int playlist_is_shuffled(void) { return atomic_load(&g_shuffle); }
void playlist_set_shuffled(int on) { atomic_store(&g_shuffle, on ? 1 : 0); }
int playlist_is_locked(void) { return atomic_load(&g_locked); }
void playlist_set_locked(int locked) { atomic_store(&g_locked, locked ? 1 : 0); }

void playlist_shuffle(void) {
	if (g_playlist_count <= 0) return;
	g_playlist_idx = rand() % g_playlist_count;
}

/* IPC read snapshot.
 * The render thread owns all live state. The IPC thread reads only the
 * snapshot, never the live structure. g_snap_dirty and the rebuild are
 * render-thread-only (no lock needed on them, same thread sets and
 * clears). g_snap_lock guards only the published snapshot, contended
 * between rare render-side republish and on-demand IPC reads. */

void playlist_mark_dirty(void) { g_snap_dirty = 1; } // render thread only

void playlist_snapshot_sync(void) {
	if (!g_snap_dirty) return; // cheap per-frame fast path
	g_snap_dirty = 0;

	/* Build the new entry copy outside the lock. Safe because the
	 * render thread is the sole writer of g_playlist, so reading
	 * it here can't race a concurrent mutation. */
	int n = g_playlist_count;
	char **copy = (n > 0) ? calloc((size_t)n, sizeof(char *)) : NULL;
	if (n > 0 && !copy) {
		g_snap_dirty = 1; // leave the old snapshot up, retry next frame
		return;
	}
	for (int i = 0; i < n; i++)
		copy[i] = strdup(g_playlist[i] ? g_playlist[i] : "");

	struct playlist_view v;
	memset(&v, 0, sizeof(v));
	v.count = g_playlist_count;
	v.index = g_playlist_idx;
	v.is_shuffled = g_shuffle;
	v.is_locked = g_locked;
	v.is_source = g_playlist_is_source ? 1 : 0;
	snprintf(v.name, sizeof(v.name), "%s", g_playlist_name);
	snprintf(v.src_path, sizeof(v.src_path), "%s", g_playlist_src_path);
	if (g_playlist_count > 0 && g_playlist_idx >= 0) {
		const char *cur = g_playlist[g_playlist_idx % g_playlist_count];
		if (cur) snprintf(v.current_path, sizeof(v.current_path), "%s", cur);
	}
	if (g_blacklisted_path)
		snprintf(v.undo_basename, sizeof(v.undo_basename), "%s",
		         path_basename(g_blacklisted_path));
	snprintf(v.undo_lane_label, sizeof(v.undo_lane_label), "%s",
	         (g_last_blacklist_lane == BLACKLIST_LANE_GLOBAL) ? " (from global)" : "");

	/* Publish. Swap under the lock. Held only for the swap. */
	pthread_mutex_lock(&g_snap_lock);
	char **old = g_snap_entries;
	int old_n = g_snap_entries_count;
	g_snap_entries = copy;
	g_snap_entries_count = n;
	g_snap_view = v;
	pthread_mutex_unlock(&g_snap_lock);

	/* Free the previous copy outside the lock. Safe. After the swap,
	 * `old` is unreachable by any reader (entry borrows are
	 * bracketed by lock and unlock_entries, and no reader can have
	 * been mid-iteration while we held the lock to swap). */
	for (int i = 0; i < old_n; i++) free(old[i]);
	free(old);
}

void playlist_snapshot_get(struct playlist_view *out) {
	if (!out) return;
	pthread_mutex_lock(&g_snap_lock);
	*out = g_snap_view;
	pthread_mutex_unlock(&g_snap_lock);
}

int playlist_snapshot_lock(int *out_index) {
	pthread_mutex_lock(&g_snap_lock);
	if (out_index) *out_index = g_snap_view.index;
	return g_snap_entries_count;
}

const char *playlist_snapshot_entry(int idx) {
	if (idx < 0 || idx >= g_snap_entries_count) return NULL;
	return g_snap_entries[idx]; // valid until playlist_snapshot_unlock
}

void playlist_snapshot_unlock(void) {
	pthread_mutex_unlock(&g_snap_lock);
}

void playlist_next(void) {
	if (g_playlist_count <= 0) return;
	if (g_shuffle) {
		/* Record where we are so we can return here, then pick random preset. */
		if (g_playlist_idx >= 0)
			nav_history_push(g_playlist[g_playlist_idx]);
		g_playlist_idx = rand() % g_playlist_count;
	} else {
		g_playlist_idx = (g_playlist_idx + 1) % g_playlist_count;
	}
}

void playlist_prev(void) {
	if (g_playlist_count <= 0) return;
	if (g_shuffle) {
		/* Walk the visited-history back. Entries are matched by path.
		 * Blacklisted or removed ones are skipped. */
		char *path;
		while ((path = nav_history_pop()) != NULL) {
			int found = -1;
			for (int i = 0; i < g_playlist_count; i++)
				if (strcmp(g_playlist[i], path) == 0) { found = i; break; }
			free(path);
			if (found >= 0) { g_playlist_idx = found; return; }
		}
		return;
	}
	g_playlist_idx = (g_playlist_idx - 1 + g_playlist_count) % g_playlist_count;
}

void playlist_set_idx(int idx) {
	if (g_playlist_count <= 0) { g_playlist_idx = -1; return; }
	if (idx < 0) idx = 0;
	if (idx >= g_playlist_count) idx = g_playlist_count - 1;
	g_playlist_idx = idx;
}

void playlist_reset_preset_timer(void) {
	clock_gettime(CLOCK_MONOTONIC, &g_timer_start);
}

double playlist_preset_age_seconds(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - g_timer_start.tv_sec)
		 + (now.tv_nsec - g_timer_start.tv_nsec) / 1e9;
}

void playlist_set_auto_advance(double seconds) {
	atomic_store(&g_auto_advance_seconds, seconds > 0 ? seconds : 0.0);
}

double playlist_auto_advance_seconds(void) {
	return atomic_load(&g_auto_advance_seconds);
}

static _Atomic int g_engine_driven = 0;

void playlist_set_engine_driven(int on) {
	atomic_store(&g_engine_driven, on ? 1 : 0);
}

/* Deferred advance. If engine owns procession, it fires own switch. */
static _Atomic int g_engine_advance_req = 0;

void playlist_request_advance(int hard_cut) {
	atomic_store(&g_engine_advance_req, hard_cut ? 2 : 1);
}

int playlist_poll_advance(void) {
	return atomic_exchange(&g_engine_advance_req, 0);
}

bool playlist_should_auto_advance(void) {
	double secs = atomic_load(&g_auto_advance_seconds);
	return secs > 0
		&& !atomic_load(&g_engine_driven)
		&& g_playlist_count > 1
		&& !atomic_load(&g_locked)
		&& playlist_preset_age_seconds() >= secs;
}

const char *playlist_transition_text(void) { return g_transition_text; }
int playlist_transition_in_progress(void) { return g_transition_pending; }

double playlist_transition_elapsed_seconds(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - g_transition_time.tv_sec)
		 + (now.tv_nsec - g_transition_time.tv_nsec) / 1e9;
}

void playlist_clear_transition(void) { g_transition_pending = 0; }

int playlist_is_globally_blacklisted(const char *path) {
	return path_in_set(path, g_global_blacklist, g_global_blacklist_count);
}

int playlist_blacklist_current(int global, char *out_name, int outlen) {
	if (g_playlist_count == 0 || g_playlist_idx < 0) return 0;

	/* Remember for undo */
	if (g_blacklisted_path) free(g_blacklisted_path);
	g_blacklisted_idx = g_playlist_idx;
	g_blacklisted_path = playlist_remove_at(g_playlist_idx);

	if (global) {
		/* Global lane. Per-playlist g_blacklist intentionally NOT touched.
		 * Global already excludes this preset everywhere */
		if (!path_in_set(g_blacklisted_path, g_global_blacklist,
		                 g_global_blacklist_count))
		{
			path_set_append(&g_global_blacklist, &g_global_blacklist_count,
			                g_blacklisted_path);
			save_global_blacklist();
		}
		g_last_blacklist_lane = BLACKLIST_LANE_GLOBAL;
		DBG("[playlist] blacklisted globally: %s", g_blacklisted_path);
	} else {
		/* Record in per-playlist blacklist so save_to and merge respect it. */
		path_set_append(&g_blacklist, &g_blacklist_count, g_blacklisted_path);
		g_last_blacklist_lane = BLACKLIST_LANE_PLAYLIST;
		DBG("[playlist] blacklisted: %s", g_blacklisted_path);
	}

	/* Advance to next. Idx now points to what was the next one */
	if (g_playlist_count > 0) {
		if (g_playlist_idx >= g_playlist_count) g_playlist_idx = 0;
		playlist_load_current(true);
	}

	if (!global) {
		/* Per-playlist lane writes the file's blacklist section.
		 * Global lane doesn't autosave. The file's canonical
		 * content didn't drift in a way that needs autosaving. */
		autosave_current();
	}

	if (out_name && outlen > 0) {
		snprintf(out_name, outlen, "%s", path_basename(g_blacklisted_path));
	}
	return 1;
}

int playlist_undo_blacklist(char *out_name, int outlen,
                            const char **out_lane_label)
{
	if (!g_blacklisted_path) return 0;

	playlist_insert_at(g_blacklisted_idx, g_blacklisted_path);
	g_playlist_idx = g_blacklisted_idx;

	/* Remove from whichever blacklist lane received this entry. */
	const char *lane_label = "";
	if (g_last_blacklist_lane == BLACKLIST_LANE_GLOBAL) {
		path_set_remove(&g_global_blacklist, &g_global_blacklist_count,
		                g_blacklisted_path);
		save_global_blacklist();
		lane_label = " (from global)";
	} else if (g_last_blacklist_lane == BLACKLIST_LANE_PLAYLIST) {
		path_set_remove(&g_blacklist, &g_blacklist_count, g_blacklisted_path);
	}

	if (out_name && outlen > 0) {
		snprintf(out_name, outlen, "%s", path_basename(g_blacklisted_path));
	}
	if (out_lane_label) *out_lane_label = lane_label;

	free(g_blacklisted_path);
	g_blacklisted_path = NULL;
	g_blacklisted_idx = -1;
	g_last_blacklist_lane = BLACKLIST_LANE_NONE;

	playlist_load_current(true);
	DBG("[playlist] undo blacklist");
	autosave_current();
	return 1;
}

int playlist_add_to_file(const char *name, const char *preset_path,
                         int prepend, char *err, int errlen)
{
	if (!name || !*name) {
		snprintf(err, errlen, "playlist name required");
		return 0;
	}
	if (!preset_path || !*preset_path) {
		snprintf(err, errlen, "no running preset");
		return 0;
	}

	if (path_in_set(preset_path, g_global_blacklist, g_global_blacklist_count)) {
		snprintf(err, errlen, "blacklisted globally: %s",
		         path_basename(preset_path));
		return 0;
	}

	char target[1024];
	playlist_path_for_name(name, target, sizeof(target));

	char **active = NULL, **blist = NULL;
	int n_active = 0, n_blist = 0;
	parse_playlist_file(target, &active, &n_active, &blist, &n_blist);

	/* Dedup vs active set. */
	if (path_in_set(preset_path, active, n_active)) {
		snprintf(err, errlen, "already in %s", name);
		path_set_free(&active, &n_active);
		path_set_free(&blist, &n_blist);
		return 0;
	}
	/* Dedup vs per-playlist blacklist section. */
	if (path_in_set(preset_path, blist, n_blist)) {
		snprintf(err, errlen, "blacklisted in %s: %s",
		         name, path_basename(preset_path));
		path_set_free(&active, &n_active);
		path_set_free(&blist, &n_blist);
		return 0;
	}

	/* Insert at front (prepend) or back (append). */
	char **new_active = malloc((size_t)(n_active + 1) * sizeof(char *));
	char *copy = strdup(preset_path);
	if (!new_active || !copy) {
		snprintf(err, errlen, "out of memory");
		free(new_active);
		free(copy);
		path_set_free(&active, &n_active);
		path_set_free(&blist, &n_blist);
		return 0;
	}
	if (prepend) {
		new_active[0] = copy;
		for (int i = 0; i < n_active; i++) new_active[i + 1] = active[i];
	} else {
		for (int i = 0; i < n_active; i++) new_active[i] = active[i];
		new_active[n_active] = copy;
	}
	free(active);
	int n_new = n_active + 1;
	int rc = write_playlist_file(target, new_active, n_new, blist, n_blist);
	path_set_free(&new_active, &n_new);
	path_set_free(&blist, &n_blist);

	if (rc != 0) {
		snprintf(err, errlen, "write failed: %s", strerror(errno));
		return 0;
	}
	return 1;
}

int playlist_auto_load(int *out_from_autosave) {
	if (out_from_autosave) *out_from_autosave = 0;
	if (!s_cfg.auto_load[0]) return 0;

	char target[1024];
	const char *al = s_cfg.auto_load;
	if (strchr(al, '/')) {
		snprintf(target, sizeof(target), "%s", al);
	} else {
		playlist_path_for_name(al, target, sizeof(target));
	}

	char ap[1024];
	int from_autosave;
	const char *load_path = playlist_autosave_prefer(target, ap, sizeof(ap),
	                                                 &from_autosave);

	if (!playlist_load_file(load_path)) return 0;

	/* Name comes from the canonical path, never the autosave sibling. */
	path_basename_no_ext(target, g_playlist_name, sizeof(g_playlist_name));
	snprintf(g_playlist_src_path, sizeof(g_playlist_src_path),
	         "%s", target);

	if (out_from_autosave) *out_from_autosave = from_autosave;
	if (from_autosave) {
		DBG("[playlist] auto-loaded '%s' from autosave (%d presets)",
		    g_playlist_name, g_playlist_count);
	} else {
		DBG("[playlist] auto-loaded '%s' (%d presets)",
		    g_playlist_name, g_playlist_count);
	}
	return 1;
}

void playlist_scan_presets_dir_fallback(void) {
	for (int ri = 0; ri < presets_dir_count(); ri++)
		playlist_scan_directory(presets_dir_at(ri));
	snprintf(g_playlist_name, sizeof(g_playlist_name), "default");
	g_playlist_src_path[0] = '\0';
}

void playlist_autosave_current(void) {
	autosave_current();
}

int playlist_autosave_path_for(const char *canonical, char *out, int outlen) {
	return autosave_path_from(canonical, out, outlen);
}

void playlist_set_name_from_path(const char *canonical) {
	path_basename_no_ext(canonical, g_playlist_name, sizeof(g_playlist_name));
}

void playlist_set_src_path(const char *canonical) {
	snprintf(g_playlist_src_path, sizeof(g_playlist_src_path),
	         "%s", canonical);
}

void playlist_clear_src_path(void) {
	g_playlist_src_path[0] = '\0';
	g_playlist_is_source = false;
}

void playlist_set_name(const char *name) {
	snprintf(g_playlist_name, sizeof(g_playlist_name), "%s", name);
}

void playlist_reset(void) {
	playlist_clear();
	g_playlist_src_path[0] = '\0';
	g_playlist_is_source = false;
	snprintf(g_playlist_name, sizeof(g_playlist_name), "default");
}

const char *playlist_pending_undo_basename(void) {
	if (!g_blacklisted_path) return NULL;
	return path_basename(g_blacklisted_path);
}

const char *playlist_pending_undo_lane_label(void) {
	return (g_last_blacklist_lane == BLACKLIST_LANE_GLOBAL) ? " (from global)" : "";
}

int playlist_pin_current_to_top(void) {
	if (g_playlist_count == 0 || g_playlist_idx < 0) return 0;
	if (g_playlist_idx == 0) return 1;
	char *running = g_playlist[g_playlist_idx];
	for (int i = g_playlist_idx; i > 0; i--) {
		g_playlist[i] = g_playlist[i - 1];
	}
	g_playlist[0] = running;
	g_playlist_idx = 0;
	return 2;
}

MODULE_REGISTER(playlist,
	.init = playlist_mod_init,
	.config_prefix = "playlist",
	.config_template =
		"playlist.auto_load=default.lst\n"
		"playlist.shuffle=1   # boolean\n"
		"playlist.duration=30   # seconds. -1 == eternal duration. avoid 0.\n",
	.config_defaults = playlist_config_defaults,
	.config_parse = playlist_config_parse,
	.config_apply = playlist_config_apply,
	.ipc_command = playlist_ipc_adapter,
	.ipc_help =
		"\nplayback:\n"
		"  next                 skip to next preset (soft cut)\n"
		"  prev                 previous preset (soft cut)\n"
		"  snap                 next preset instantly (no soft cut)\n"
		"  list presets         presets in the current playlist\n"
		"  list playlists [all] saved playlists by last-loaded\n"
		"\ntoggles:\n"
		"  shuffle [on|off]     toggle or set shuffle\n"
		"  lock [on|off]        toggle or set preset lock\n"
		"  dur <seconds>        preset auto-advance duration\n"
		"\nplaylist 'pl':\n"
		"  blacklist            remove current preset from playlist\n"
		"  blacklist-global     also exclude from every playlist\n"
		"  undo                 undo last blacklist (either lane)\n"
		"  playlist load [name|path]   load saved playlist\n"
		"  playlist save [name|path]   save current playlist (default: current name)\n"
		"  playlist new <n> [path]  create playlist (empty or from dir/file)\n"
		"  playlist merge [name|path]  merge into current\n"
		"  playlist add <n>         append running preset to <n>\n"
		"  playlist add-pin <n>     prepend running preset to <n>\n"
		"  playlist pin                move running preset to top of loaded playlist\n"
		"  Bare names like 'jams' resolve under the playlists dir; use a path with / to bypass.\n");
