// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file backend.c
 * @brief `display` config slice.
 *
 * Key stays `display` though the module is `backend`. */

#include "backend.h"
#include "module_registry.h"
#include <stdio.h>
#include <string.h>

static struct backend_config s_cfg;

static void backend_config_defaults(void) {
	snprintf(s_cfg.mode, sizeof(s_cfg.mode), "auto");
}

static int backend_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "mode")) {
		snprintf(s_cfg.mode, sizeof(s_cfg.mode), "%s", val);
		return 1;
	}
	return 0;
}

const char *backend_mode(void) { return s_cfg.mode; }

MODULE_REGISTER(backend,
	.config_prefix = "display",
	.config_template =
		"display.mode=auto   # auto|wallpaper|windowed\n",
	.config_defaults = backend_config_defaults,
	.config_parse = backend_config_parse);
