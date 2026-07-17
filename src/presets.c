// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file presets.c
 * @brief `presets` config slice.
 *
 * Splits the configured directory value into roots and unions each
 * engine's advertised extensions with any the config adds. */

#include "presets.h"
#include "module_registry.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static struct presets_config s_cfg;

/* Append extensions onto the union. */
static void presets_collect_ext(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (!d->file_extensions || !d->file_extensions[0]) return;
	size_t len = strlen(s_cfg.ext);
	snprintf(s_cfg.ext + len, sizeof(s_cfg.ext) - len,
	         "%s%s", len ? " " : "", d->file_extensions);
}

static void presets_config_defaults(void) {
	s_cfg.dir[0] = '\0';
	s_cfg.root_count = 0;
	s_cfg.ext[0] = '\0';
	module_registry_visit(presets_collect_ext, NULL);
}

static int presets_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "dir")) {
		/* Keep raw value for display, split it by colon into roots, strip
		 * trailing slashes and drop empty tokens. */
		snprintf(s_cfg.dir, sizeof(s_cfg.dir), "%s", val);
		s_cfg.root_count = 0;
		const char *p = val;
		while (*p && s_cfg.root_count < PRESETS_MAX_ROOTS) {
			while (*p == ':') p++; // skip separators
			const char *start = p;
			while (*p && *p != ':') p++;
			size_t len = (size_t)(p - start);
			if (len == 0) continue; // empty token
			if (len >= sizeof(s_cfg.roots[0]))
				len = sizeof(s_cfg.roots[0]) - 1;
			char *dst = s_cfg.roots[s_cfg.root_count];
			memcpy(dst, start, len);
			dst[len] = '\0';
			path_strip_trailing_slashes(dst);
			if (dst[0]) s_cfg.root_count++; // skip if slash-strip emptied it
		}
		return 1;
	}
	if (!strcmp(k, "extensions")) {
		size_t len = strlen(s_cfg.ext);
		snprintf(s_cfg.ext + len, sizeof(s_cfg.ext) - len,
		         "%s%s", len ? " " : "", val);
		return 1;
	}
	return 0;
}

const char *presets_dir(void) { return s_cfg.dir; }
int presets_dir_count(void) { return s_cfg.root_count; }
const char *presets_dir_at(int i) {
	if (i < 0 || i >= s_cfg.root_count) return NULL;
	return s_cfg.roots[i];
}
const char *preset_extensions(void) { return s_cfg.ext; }

int preset_ext_match(const char *name) {
	return suffix_in_list(name, s_cfg.ext);
}

MODULE_REGISTER(presets,
	.config_prefix = "presets",
	.config_template = "presets.dir=/usr/share/projectM/presets:/usr/local/share/projectM/presets\n",
	.config_defaults = presets_config_defaults,
	.config_parse = presets_config_parse);
