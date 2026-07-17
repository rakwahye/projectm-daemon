// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file scene.c
 * @brief Frame composite sequence.
 *
 * Fixed per-frame order: clear, engine render, overlay burn, background
 * fill, effect phase, present phase, overlay. Clears to transparent,
 * not black, so the later hole-fill can paint the background into
 * untouched alpha. Ends with `glFinish` so the caller can lock the
 * buffer. */

#define _POSIX_C_SOURCE 200809L

#include "visualizer.h"
#include "scene_router.h"
#include "overlay.h"
#include "gl_quad.h"
#include "color.h"
#include "module_registry.h"
#include "scene.h"
#include <stdint.h>
#include <string.h>
#include <GLES3/gl3.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

static struct scene_config s_cfg;

static void scene_config_defaults(void) {
	s_cfg.bg_r = 0.0f;
	s_cfg.bg_g = 0.0f;
	s_cfg.bg_b = 0.0f;
	s_cfg.bg_alpha = 1.0f;
}

static int scene_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "bg_color")) {
		color_parse_hex(val, &s_cfg.bg_r, &s_cfg.bg_g, &s_cfg.bg_b,
		                &s_cfg.bg_alpha);
		return 1;
	}
	return 0;
}

struct scene_phase_ctx { int phase; int w; int h; };

static void scene_render_phase_cb(const struct module_descriptor *d, void *ud) {
	struct scene_phase_ctx *c = ud;
	if (d->render && d->render_phase == c->phase)
		d->render(c->w, c->h);
}

static void scene_render_phase(int phase, int w, int h) {
	struct scene_phase_ctx c = { phase, w, h };
	module_registry_visit(scene_render_phase_cb, &c);
}

void scene_render(int width, int height) {
	struct visualizer *vis = visualizer_active();

	overlay_tick();

	/* Transparent, not black: `gl_quad_fill_holes` paints the bg color into
	 * whatever alpha the frame leaves untouched. */
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	const struct frame_pacer *pacer = module_active_pacer();
	if (pacer && pacer->measure_begin) pacer->measure_begin();
	vis->render(vis);
	if (pacer && pacer->measure_end) pacer->measure_end();

	if (overlay_burn_enabled()) {
		uint32_t tex = 0;
		int x = 0, y = 0, w = 0, h = 0;
		float alpha = 1.0f;
		if (overlay_poll_burn(&tex, &x, &y, &w, &h, &alpha))
			scene_router_deposit(vis, tex, x, y, w, h, width, height, alpha);
	}

	gl_quad_fill_holes(s_cfg.bg_r, s_cfg.bg_g, s_cfg.bg_b, s_cfg.bg_alpha);

	scene_render_phase(RENDER_PHASE_EFFECT, width, height);

	/* A present-phase module routes its own deposit-intent. */
	scene_render_phase(RENDER_PHASE_PRESENT, width, height);

	/* In-scene overlay. A FRONT-layer overlay commits its own surface and
	 * this is a no-op. */
	overlay_render_present(width, height);

	/* Flush so the caller can safely lock the front buffer */
	glFinish();
}

MODULE_REGISTER(scene,
	.config_prefix = "scene",
	.config_template =
		"scene.bg_color=00000000\n",
	.config_defaults = scene_config_defaults,
	.config_parse = scene_config_parse);
