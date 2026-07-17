// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file pending.c
 * @brief Pending action dispatch.
 *
 * Maps each queued action to its module call, run on the render thread
 * so the calls touch engine and playlist state without locking. */

#define _POSIX_C_SOURCE 200809L

#include "pending.h"
#include "runtime.h"
#include "config.h"
#include "cli.h"
#include "playlist.h"
#include "visualizer.h"
#include "module_registry.h"
#include <stdio.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

/* Push config to the visualizer and reapply the pacer. */
static void apply_visualizer_config(struct rt *rt) {
	rt->vis->apply_config(rt->vis);
	const struct frame_pacer *pacer = module_active_pacer();
	if (pacer && pacer->repush) pacer->repush();
}

/* Reload config from disk and make it live. */
static void apply_config_reload(struct rt *rt) {
	config_load();
	cli_apply_overrides(false); // CLI overrides win

	const struct frame_pacer *pacer = module_active_pacer();
	if (pacer && pacer->reload) pacer->reload();

	apply_visualizer_config(rt);
	config_apply_all();
	playlist_reload();
	DBG("[config] reloaded");
}

void apply_pending(struct rt *rt) {
	int action = atomic_exchange(&rt->pending.action, PENDING_NONE);
	switch (action) {
	case PENDING_NONE:
		break;
	case PENDING_LOAD:
		pthread_mutex_lock(&rt->pending.lock);
		if (rt->pending.path[0]) {
			struct visualizer *vis = visualizer_ensure_for(rt->pending.path);
			if (vis && vis->load_preset(vis, rt->pending.path, true))
				DBG("[ipc] loaded preset: %s", rt->pending.path);
			else
				DBG("[ipc] load failed: %s", rt->pending.path);
			rt->pending.path[0] = '\0';
		}
		pthread_mutex_unlock(&rt->pending.lock);
		break;
	case PENDING_RELOAD:
		apply_config_reload(rt);
		break;
	default:
		playlist_apply_pending(rt, action);
		break;
	}

	if (action != PENDING_NONE) playlist_mark_dirty();
	playlist_snapshot_sync();
}
