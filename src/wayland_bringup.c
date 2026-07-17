// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wayland_bringup.c
 * @brief Wayland bring-up.
 *
 * Owns the role CLI options and realizes a single Wayland output at
 * the requested shell role. */

#define _POSIX_C_SOURCE 200809L

#include "backend_bringup.h"
#include "renderer.h"
#include "output.h"
#include "backend.h"
#include "cli.h"
#include "module_registry.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (0)

static char s_role_spec[16] = "";

static int h_display(const char *optarg)
{
	snprintf(s_role_spec, sizeof(s_role_spec), "%s", optarg ? optarg : "");
	return 0;
}
static int h_windowed(const char *optarg)
{
	(void)optarg;
	snprintf(s_role_spec, sizeof(s_role_spec), "windowed");
	return 0;
}

static int wayland_register_cli(void)
{
	static const struct cli_option opts[] = {{
		.long_name = "display", .has_arg = CLI_REQUIRED_ARG,
		.help = "surface role: auto | wallpaper | windowed | headless",
		.handler = h_display
	},{
		.long_name = "windowed", .short_name = 'w', .has_arg = CLI_NO_ARG,
		.help = "shorthand for --display windowed",
		.handler = h_windowed
	},};
	return cli_register("wayland", opts, 2);
}

MODULE_REGISTER(wayland_backend, .register_cli = wayland_register_cli);

bool backend_plan(struct rt *rt, struct backend_plan *out)
{
	(void)rt;
	const char *spec = s_role_spec;
	const char *mode = spec[0] ? spec : backend_mode();
	out->platform = (mode && !strcmp(mode, "headless"))
	                ? RENDER_SURFACELESS : RENDER_GBM;
	return true;
}

/* Resolve role string -> surface role. */
static output_role_t resolve_role(const char *spec)
{
	if (!spec || !*spec) return OUTPUT_ROLE_AUTO;
	if (!strcmp(spec, "auto")) return OUTPUT_ROLE_AUTO;
	if (!strcmp(spec, "wallpaper")) return OUTPUT_ROLE_WALLPAPER;
	if (!strcmp(spec, "windowed")) return OUTPUT_ROLE_WINDOWED;
	fprintf(stderr, "[wiring] unknown present mode '%s', defaulting"
	        " to auto\n", spec);
	return OUTPUT_ROLE_AUTO;
}

bool backend_bringup(struct rt *rt, struct renderer *prod,
		             struct backend_outputs *out)
{
	(void)rt;
	memset(out, 0, sizeof(*out));

	const char *spec = s_role_spec;
	const char *mode = spec[0] ? spec : backend_mode();

	if (mode && !strcmp(mode, "headless")) {
		struct output *hl = headless_create();
		if (!hl || !hl->init(hl, prod)) {
			fprintf(stderr, "[wiring] headless init failed\n");
			if (hl) hl->destroy(hl);
			return false;
		}
		out->items[0] = hl;
		out->n = 1;
		out->master_w = hl->get_width(hl);
		out->master_h = hl->get_height(hl);
		out->master_hz = hl->get_refresh_hz ? hl->get_refresh_hz(hl) : 0;
		out->teardown = NULL;
		DBG("[wiring] headless backend up");
		return true;
	}

	/* CLI (--display / -w) wins over config.display_mode. Resolution is
	 * deferred to here so config has already supplied its default. */
	output_role_t role = resolve_role(mode);

	struct output *wl = output_wayland_create(role);
	if (!wl) {
		fprintf(stderr, "[wiring] output create failed\n");
		return false;
	}

	/* Init against the renderer. Borrows gbm_dev + EGL ctx, connects
	 * Wayland, blocks on first configure, learns real dimensions. */
	if (!wl->init(wl, prod)) {
		fprintf(stderr, "[wiring] output init failed\n");
		wl->destroy(wl);
		return false;
	}

	out->items[0] = wl;
	out->n = 1;
	out->master_w = wl->get_width(wl);
	out->master_h = wl->get_height(wl);
	out->master_hz = wl->get_refresh_hz ? wl->get_refresh_hz(wl) : 0;
	out->teardown = NULL;

	DBG("[wiring] wayland backend up");
	return true;
}
