// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file scene_router.c
 * @brief Deposit routing.
 *
 * The fallback that lets an engine leave `deposit` NULL. Routes to
 * the engine's `deposit` when present, else blits a quad. */

#include "scene_router.h"
#include "visualizer.h"
#include "gl_quad.h"

void scene_router_deposit(struct visualizer *vis,
                       uint32_t tex, int x, int y, int w, int h,
                       int surf_w, int surf_h, float alpha)
{
	if (vis && vis->deposit) {
		vis->deposit(vis, tex, x, y, w, h);
		(void)surf_w;
		(void)surf_h;
		(void)alpha;
	} else {
		gl_quad_blit(tex, x, y, w, h, surf_w, surf_h, alpha);
	}
}
