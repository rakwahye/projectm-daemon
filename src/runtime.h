// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file runtime.h
 * @brief Runtime coordination object and pending mailbox.
 *
 * The `rt` handle main seeds and threads to the render callbacks and
 * to modules at init: active engine, output size, frame count, and
 * the deferred-action mailbox that carries IPC requests to the render
 * thread. */

#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct visualizer;
struct module_descriptor;

/** Deferred render-thread actions. */
enum pending_action {
	PENDING_NONE = 0, PENDING_NEXT, PENDING_PREV, PENDING_SNAP, PENDING_LOAD,
	PENDING_RELOAD, PENDING_BLACKLIST, PENDING_BLACKLIST_GLOBAL, PENDING_UNDO,
	PENDING_PLAYLIST_LOAD, PENDING_PLAYLIST_SAVE, PENDING_PLAYLIST_NEW,
	PENDING_PLAYLIST_MERGE, PENDING_PLAYLIST_RESCAN, PENDING_TOGGLE_SHUFFLE,
	PENDING_TOGGLE_LOCK, PENDING_SET_DURATION, PENDING_PLAYLIST_ADD,
	PENDING_PLAYLIST_ADD_PIN, PENDING_PLAYLIST_PIN
};

struct rt {
	int frame_count; // presented-frame counter

	struct visualizer *vis; // active visualizer
	const struct module_descriptor *vis_desc; // live engine

	/** Output dimensions. 0 until the output configures a size. */
	atomic_int output_w;
	atomic_int output_h;

	/** Deferred-action mailbox. IPC handlers queue. The render thread
	 * drains once per frame. `lock` guards `path` and `duration`.
	 * `action` is atomic. */
	struct {
		atomic_int action;
		char path[1024];
		double duration;
		pthread_mutex_t lock;
	} pending;
};

/** Queue an action with no payload. */
static inline void rt_pending_post(struct rt *rt, int action) {
	atomic_store(&rt->pending.action, action);
}

/** Queue an action carrying a path or name. Payload is set under lock,
 * the action published last, so the render thread never pairs a fresh
 * action with a stale payload. */
static inline void rt_pending_post_path(struct rt *rt, int action,
                                        const char *path) {
	pthread_mutex_lock(&rt->pending.lock);
	snprintf(rt->pending.path, sizeof(rt->pending.path), "%s", path ? path : "");
	pthread_mutex_unlock(&rt->pending.lock);
	atomic_store(&rt->pending.action, action);
}

/** Queue an action carrying two strings packed `a\0b` in one buffer.
 * `b` may be empty. The executor reads `a`, then `a+strlen(a)+1` for
 * `b`. */
static inline void rt_pending_post_pair(struct rt *rt, int action,
                                        const char *a, const char *b) {
	pthread_mutex_lock(&rt->pending.lock);
	int n = snprintf(rt->pending.path, sizeof(rt->pending.path), "%s", a ? a : "");
	if (n >= 0 && (size_t)n + 1 < sizeof(rt->pending.path))
		snprintf(rt->pending.path + n + 1,
		         sizeof(rt->pending.path) - (size_t)n - 1, "%s", b ? b : "");
	pthread_mutex_unlock(&rt->pending.lock);
	atomic_store(&rt->pending.action, action);
}

/** Queue an action carrying a duration. */
static inline void rt_pending_post_duration(struct rt *rt, int action,
                                            double duration) {
	pthread_mutex_lock(&rt->pending.lock);
	rt->pending.duration = duration;
	pthread_mutex_unlock(&rt->pending.lock);
	atomic_store(&rt->pending.action, action);
}

bool setup_visualizer(int width, int height, int vrefresh_hz,
                      const void *pace_ctx);

void main_set_output_size(int w, int h);

#ifdef __cplusplus
}
#endif

#endif
