// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file presets.h
 * @brief Preset roots and file-type matching.
 *
 * The directory roots scanned for loadable presets, and the extension
 * set a preset filename must match. Reached through the accessors. */

#ifndef PRESETS_H
#define PRESETS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PRESETS_MAX_ROOTS 16

struct presets_config {
	char dir[512]; // raw configured value (for display)
	char ext[256];
	char roots[PRESETS_MAX_ROOTS][512]; // dir split into roots
	int root_count;
};

/** Raw configured `presets.dir` value. */
const char *presets_dir(void);
/** Parsed-root count. */
int presets_dir_count(void);
/** The i-th parsed root. @returns NULL if out of range. */
const char *presets_dir_at(int i);

/** The advertised extension set. Engines append theirs at registration
 * (compile-time). Config key `presets.extensions` appends more. */
const char *preset_extensions(void);
/** @returns 1 if `name` carries an advertised extension. */
int preset_ext_match(const char *name);

#ifdef __cplusplus
}
#endif

#endif
