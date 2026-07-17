// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file visualizer.c
 * @brief Engine selection and bring-up.
 *
 * Swaps the live engine to the one that owns a given preset, tearing the
 * previous down first and publishing the new one only after a clean
 * init. Render-thread only. */

#define _POSIX_C_SOURCE 200809L

#include "visualizer.h"
#include "module_registry.h"

bool visualizer_bringup(struct visualizer *vis,
                        const struct module_descriptor *desc, int w, int h) {
	if (!vis || !vis->init(vis, w, h))
		return false;
	/* Publish only after a successful init, so a failed bring-up never
	 * leaves a half-live state. */
	visualizer_set_active(vis, desc);
	return true;
}

struct visualizer *visualizer_ensure_for(const char *path) {
	const struct module_descriptor *want = module_engine_for(path);
	struct visualizer *live = visualizer_active();
	const struct module_descriptor *live_desc = visualizer_active_desc();

	if (live && want && want != live_desc) {
		live->destroy(live);
		visualizer_set_active(NULL, NULL);
		live = NULL;
	}

	if (!live && want) {
		struct visualizer *nv = want->create();
		if (!nv)
			return NULL;
		int w = 0, h = 0;
		visualizer_output_size(&w, &h);
		if (!visualizer_bringup(nv, want, w, h)) {
			visualizer_set_active(NULL, NULL);
			return NULL;
		}
		const struct frame_pacer *pacer = module_active_pacer();
		if (pacer && pacer->repush)
			pacer->repush();
		live = visualizer_active();
	}

	return live;
}
