// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file backend_bringup.h
 * @brief Two-phase output bring-up: plan the platform, then realize outputs. */

#ifndef BACKEND_BRINGUP_H
#define BACKEND_BRINGUP_H

#include "output.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rt;
struct renderer;

#define BACKEND_MAX_OUTPUTS 4

/** The chosen render platform, decided before `renderer_init` without
 * touching hardware. */
struct backend_plan {
	enum render_platform_kind platform;
};

bool backend_plan(struct rt *rt, struct backend_plan *out);

struct backend_outputs {
	struct output *items[BACKEND_MAX_OUTPUTS];
	int n;

	/** Opaque pacing context for the frame pacer. NULL when the output
	 * paces itself. Producer and pacer agree the type privately. */
	const void *pace_ctx;

	/** Size the renderer draws at and the refresh it paces to. */
	int master_w;
	int master_h;
	int master_hz; // 0 = unknown

	void *teardown_ctx;
	void (*teardown)(void *ctx);
};

bool backend_bringup(struct rt *rt, struct renderer *prod,
                     struct backend_outputs *out);

#ifdef __cplusplus
}
#endif

#endif
