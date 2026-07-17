// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file coord.h
 * @brief Length parsing and placement.
 *
 * A bare number is a fraction of the display dimension; a `px` suffix is
 * pixels. One grammar, shared so it can't drift. */

#ifndef COORD_H
#define COORD_H

#include <stddef.h>

typedef enum {
	COORD_UNIT_FRAC = 0, // fraction of display dimensions
	COORD_UNIT_PX,
} coord_unit_t;

typedef struct {
	double value;
	coord_unit_t unit;
	int present; // 0 = default. 1 = parsed value.
} coord_length_t;

/** Unspecified sentinel. Use in struct inits when key not set. */
static inline coord_length_t coord_length_unspecified(void) {
	coord_length_t r;
	r.value = 0.0;
	r.unit = COORD_UNIT_FRAC;
	r.present = 0;
	return r;
}

/** Concrete-value constructor for defaults and tests. */
static inline coord_length_t coord_length_frac(double v) {
	coord_length_t r = { v, COORD_UNIT_FRAC, 1 };
	return r;
}

/** Concrete-value constructor for defaults and tests. */
static inline coord_length_t coord_length_px(double v) {
	coord_length_t r = { v, COORD_UNIT_PX, 1 };
	return r;
}

/** Parse a length string.
 * @returns -1 on parse error, leaving `out` untouched. */
int coord_length_parse(const char *str, coord_length_t *out);

/** Format for logging and re-emission. `bufsz` >= 32 recommended.
 * Unspecified renders as "auto". */
void coord_length_format(coord_length_t len, char *buf, size_t bufsz);

/** Convert to pixels using relevant display dimension.
 * @returns Rounded pixel value. -1 = Unspecified. */
int coord_length_to_px(coord_length_t len, int display_dim);

/** Where on rect the position refers. Default is CENTER. */
typedef enum {
	COORD_ANCHOR_CENTER = 0,
	COORD_ANCHOR_TOP_LEFT,
	COORD_ANCHOR_TOP,
	COORD_ANCHOR_TOP_RIGHT,
	COORD_ANCHOR_LEFT,
	COORD_ANCHOR_RIGHT,
	COORD_ANCHOR_BOTTOM_LEFT,
	COORD_ANCHOR_BOTTOM,
	COORD_ANCHOR_BOTTOM_RIGHT,
} coord_anchor_t;

int coord_anchor_parse(const char *str, coord_anchor_t *out);
const char *coord_anchor_name(coord_anchor_t a);

/** Aspect mode. */
typedef enum {
	COORD_ASPECT_FIT = 0,
	COORD_ASPECT_FILL,
	COORD_ASPECT_STRETCH,
	COORD_ASPECT_NATIVE,
} coord_aspect_t;

int coord_aspect_parse(const char *str, coord_aspect_t *out);
const char *coord_aspect_name(coord_aspect_t a);

/** Resolved pixel rectangle on the display. `x` and `y` are TOP-LEFT.
 * Negative is legal. */
typedef struct {
	int x, y, w, h;
} coord_rect_t;

/** Resolve placement spec to concrete pixel rect.
 * Unspecified position = center. Unspecified size = full display.
 * src_w/h = 0 falls back to STRETCH. */
void coord_resolve_rect(coord_length_t pos_x, coord_length_t pos_y,
  coord_length_t size_w, coord_length_t size_h, coord_anchor_t anchor,
  coord_aspect_t aspect, int display_w, int display_h, int src_w,
  int src_h, coord_rect_t *out);

#endif
