// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file render_params.c
 * @brief `render` config slice.
 *
 * Parses and stores the shared mesh size and frame rate. */

#include "render_params.h"
#include "module_registry.h"
#include <stdlib.h>
#include <string.h>

static struct render_params_config s_cfg;

static void render_params_config_defaults(void) {
	s_cfg.mesh_x = 48;
	s_cfg.mesh_y = 32;
	s_cfg.fps = 60;
}

static int render_params_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "mesh_x")) { s_cfg.mesh_x = atoi(val); return 1; }
	if (!strcmp(k, "mesh_y")) { s_cfg.mesh_y = atoi(val); return 1; }
	if (!strcmp(k, "fps")) { s_cfg.fps = atoi(val); return 1; }
	return 0;
}

int render_mesh_x(void) { return s_cfg.mesh_x; }
int render_mesh_y(void) { return s_cfg.mesh_y; }
int render_fps(void) { return s_cfg.fps; }

MODULE_REGISTER(render_params,
	.config_prefix = "render",
	.config_template =
		"render.mesh_x=48   # integer\n"
		"render.mesh_y=32   # integer\n"
		"render.fps=60   # integer\n",
	.config_defaults = render_params_config_defaults,
	.config_parse = render_params_config_parse);
