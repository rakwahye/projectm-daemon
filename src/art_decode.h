// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file art_decode.h
 * @brief Album-art image decode. */

#ifndef ART_DECODE_H
#define ART_DECODE_H

#include <cairo/cairo.h>

typedef enum {
	ART_DECODE_OK = 0,
	ART_DECODE_OPEN_FAILED,
	ART_DECODE_BAD_DIMENSIONS,
	ART_DECODE_DECODE_FAILED,
	ART_DECODE_OOM,
} art_decode_status;

typedef struct {
	art_decode_status status;
	int width, height;
} art_decode_info;

/** Decode a PNG or JPEG to an ARGB32 cairo surface. `info` may be NULL.
 * When set, it reports the failure mode and, for a reject, the
 * dimensions the header declared.
 * @returns NULL on failure. */
cairo_surface_t *art_decode_image(const char *path, art_decode_info *info);

#endif
