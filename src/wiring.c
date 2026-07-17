// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wiring.c
 * @brief Daemon assembly.
 *
 * Brings up the renderer, runs backend bring-up, sizes the pipeline
 * from the resolved outputs, then runs the loop with the runtime
 * object threaded onto its user pointer. */

#define _POSIX_C_SOURCE 200809L

#include "renderer.h"
#include "output.h"
#include "loop.h"
#include "wayland.h"
#include "wiring.h"
#include "wiring_render.h"
#include "backend_bringup.h"
#include "runtime.h"
#include "scene.h"
#include "gl_quad.h"
#include "module_registry.h"
#include "visualizer.h"
#include "audio.h"
#include "playlist.h"
#include "overlay.h"
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>

extern int g_debug;
extern _Atomic sig_atomic_t g_running;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

static void wiring_render_init_cb(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->render_init) d->render_init();
}

static void wiring_render_destroy_cb(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->render_destroy) d->render_destroy();
}

/* Provisional size until the primary output reports its mode. */
#define PROVISIONAL_W 1280
#define PROVISIONAL_H 720

/* Resize the renderer to the master size, then bring up GL, visualizer,
 * scene, and overlay. Done once. */
static bool pipeline_finalize(struct renderer *prod, int master_w,
			      int master_h, int vrefresh_hz,
			      const void *pace_ctx)
{
	if (master_w <= 0 || master_h <= 0) {
		fprintf(stderr, "[wiring] bad master size %dx%d\n", master_w, master_h);
		return false;
	}
	if (!renderer_resize(prod, master_w, master_h)) {
		fprintf(stderr, "[wiring] renderer_resize to %dx%d failed\n",
				master_w, master_h);
		return false;
	}

	if (!gl_quad_init()) {
		fprintf(stderr, "[wiring] gl_quad_init failed\n");
		return false;
	}
	module_registry_visit(wiring_render_init_cb, NULL);

	/* Wayland globals for FRONT-mode overlay. `NULL` when no Wayland output
	 * is present, in which case modules downgrade FRONT to BACK. */
	struct wl_compositor *wl_comp = wayland_get_compositor();
	struct wl_shm *wl_shm = wayland_get_shm();
	struct wl_output *wl_out = wayland_get_output();
	struct zwlr_layer_shell_v1 *wl_layer_shell = wayland_get_layer_shell();

	if (!setup_visualizer(master_w, master_h, vrefresh_hz, pace_ctx)) {
		fprintf(stderr, "[wiring] visualizer setup failed\n");
		module_registry_visit_reverse(wiring_render_destroy_cb, NULL);
		gl_quad_destroy();
		return false;
	}

	main_set_output_size(master_w, master_h);

	/* Overlay at master size. Only clamp FRONT->BACK when layer-shell
	 * isn't available (the spec list has no Wayland output), otherwise
	 * let the configured layer through. */
	if (!wl_layer_shell) overlay_clamp_layer_to_back("no layer-shell");
	if (!overlay_init(wl_comp, wl_shm, wl_layer_shell, wl_out,
					  master_w, master_h))
		fprintf(stderr, "[wiring] overlay init failed (non-fatal)\n");

	DBG("[wiring] render pipeline up at %dx%d", master_w, master_h);
	return true;
}

int run_daemon(_Atomic sig_atomic_t *running, struct rt *rt)
{
	struct renderer prod;

	struct backend_plan plan = { .platform = RENDER_GBM };
	if (!backend_plan(rt, &plan)) {
		fprintf(stderr, "[wiring] backend_plan failed\n");
		return 1;
	}

	/* Renderer first, provisional size. Outputs borrow it. */
	if (!renderer_init(&prod, PROVISIONAL_W, PROVISIONAL_H, plan.platform)) {
		fprintf(stderr, "[wiring] renderer_init failed\n");
		return 1;
	}

	struct backend_outputs bc;
	if (!backend_bringup(rt, &prod, &bc)) {
		renderer_cleanup(&prod);
		return 1;
	}

	if (!pipeline_finalize(&prod, bc.master_w, bc.master_h, bc.master_hz,
						   bc.pace_ctx)) {
		for (int i = 0; i < bc.n; i++) bc.items[i]->destroy(bc.items[i]);
		renderer_cleanup(&prod);
		if (bc.teardown) bc.teardown(bc.teardown_ctx);
		return 1;
	}

	struct loop lp = {
		.renderer = &prod,
		.outputs = bc.items,
		.n_outputs = bc.n,
		.prologue = wiring_render_prologue,
		.epilogue = wiring_render_epilogue,
		.user = rt,
	};

	int rc = loop_run(&lp, running);

	/* GL-resource teardown with the renderer's context still current */
	eglMakeCurrent(prod.egl_dpy, prod.egl_surf, prod.egl_surf, prod.egl_ctx);
	overlay_destroy();
	module_registry_visit_reverse(wiring_render_destroy_cb, NULL);
	gl_quad_destroy();
	rt->vis->destroy(rt->vis);

	for (int i = 0; i < bc.n; i++) bc.items[i]->destroy(bc.items[i]);
	renderer_cleanup(&prod);
	if (bc.teardown) bc.teardown(bc.teardown_ctx);
	return rc;
}
