// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file coord.c
 * @brief Length parse and placement resolution. */

#include "coord.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int coord_length_parse(const char *str, coord_length_t *out) {
	if (!str || !out) return -1;

	while (*str && isspace((unsigned char)*str)) str++;
	if (!*str) return -1;

	/* Parse the numeric body. Consume longest valid prefix. */
	char *endp = NULL;
	double v = strtod(str, &endp);
	if (endp == str) return -1;
	if (!isfinite(v)) return -1;
	if (v < 0.0) return -1;

	/* Suffix. Optional "px", and optionally surrounded by whitespace. */
	coord_unit_t unit = COORD_UNIT_FRAC;
	const char *p = endp;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p) {
		if (p[0] == 'p' && p[1] == 'x') {
			unit = COORD_UNIT_PX;
			p += 2;
			while (*p && isspace((unsigned char)*p)) p++;
			if (*p) return -1; // trash after "px"
		} else {
			return -1;
		}
	}

	out->value = v;
	out->unit = unit;
	out->present = 1;
	return 0;
}

void coord_length_format(coord_length_t len, char *buf, size_t bufsz) {
	if (!buf || bufsz == 0) return;
	if (!len.present) {
		snprintf(buf, bufsz, "auto");
	} else if (len.unit == COORD_UNIT_PX) {
		snprintf(buf, bufsz, "%.0fpx", len.value);
	} else {
		snprintf(buf, bufsz, "%.4f", len.value);
	}
}

int coord_length_to_px(coord_length_t len, int display_dim) {
	if (!len.present) return -1;
	if (len.unit == COORD_UNIT_PX) {
		return (int)(len.value + 0.5);
	}
	if (display_dim <= 0) return 0;
	return (int)(len.value * (double)display_dim + 0.5);
}

static const struct {
	coord_anchor_t a;
	const char *name;
} g_anchor_names[] = {
	{ COORD_ANCHOR_CENTER, "center" },
	{ COORD_ANCHOR_TOP_LEFT, "top-left" },
	{ COORD_ANCHOR_TOP, "top" },
	{ COORD_ANCHOR_TOP_RIGHT, "top-right" },
	{ COORD_ANCHOR_LEFT, "left" },
	{ COORD_ANCHOR_RIGHT, "right" },
	{ COORD_ANCHOR_BOTTOM_LEFT, "bottom-left" },
	{ COORD_ANCHOR_BOTTOM, "bottom" },
	{ COORD_ANCHOR_BOTTOM_RIGHT, "bottom-right" },
};

int coord_anchor_parse(const char *str, coord_anchor_t *out) {
	if (!str || !out) return -1;
	/* Skip leading whitespace, tolerate trailing whitespace. */
	while (*str && isspace((unsigned char)*str)) str++;
	size_t n = strlen(str);
	while (n > 0 && isspace((unsigned char)str[n - 1])) n--;
	for (size_t i = 0; i < sizeof(g_anchor_names) / sizeof(g_anchor_names[0]); i++) {
		if (strlen(g_anchor_names[i].name) == n
		    && strncmp(str, g_anchor_names[i].name, n) == 0) {
			*out = g_anchor_names[i].a;
			return 0;
		}
	}
	return -1;
}

const char *coord_anchor_name(coord_anchor_t a) {
	for (size_t i = 0; i < sizeof(g_anchor_names) / sizeof(g_anchor_names[0]); i++) {
		if (g_anchor_names[i].a == a) return g_anchor_names[i].name;
	}
	return "center";
}

static const struct {
	coord_aspect_t a;
	const char *name;
} g_aspect_names[] = {
	{ COORD_ASPECT_FIT, "fit" },
	{ COORD_ASPECT_FILL, "fill" },
	{ COORD_ASPECT_STRETCH, "stretch" },
	{ COORD_ASPECT_NATIVE, "native" },
};

int coord_aspect_parse(const char *str, coord_aspect_t *out) {
	if (!str || !out) return -1;
	while (*str && isspace((unsigned char)*str)) str++;
	size_t n = strlen(str);
	while (n > 0 && isspace((unsigned char)str[n - 1])) n--;
	for (size_t i = 0; i < sizeof(g_aspect_names) / sizeof(g_aspect_names[0]); i++) {
		if (strlen(g_aspect_names[i].name) == n
		    && strncmp(str, g_aspect_names[i].name, n) == 0) {
			*out = g_aspect_names[i].a;
			return 0;
		}
	}
	return -1;
}

const char *coord_aspect_name(coord_aspect_t a) {
	for (size_t i = 0; i < sizeof(g_aspect_names) / sizeof(g_aspect_names[0]); i++) {
		if (g_aspect_names[i].a == a) return g_aspect_names[i].name;
	}
	return "fit";
}

static void apply_anchor(coord_anchor_t a,
                         int anchor_x, int anchor_y,
                         int rect_w, int rect_h,
                         int *out_tl_x, int *out_tl_y)
{
	int dx, dy;
	switch (a) {
		case COORD_ANCHOR_TOP_LEFT: dx = 0; dy = 0; break;
		case COORD_ANCHOR_TOP: dx = rect_w/2; dy = 0; break;
		case COORD_ANCHOR_TOP_RIGHT: dx = rect_w; dy = 0; break;
		case COORD_ANCHOR_LEFT: dx = 0; dy = rect_h/2; break;
		case COORD_ANCHOR_RIGHT: dx = rect_w; dy = rect_h/2; break;
		case COORD_ANCHOR_BOTTOM_LEFT: dx = 0; dy = rect_h; break;
		case COORD_ANCHOR_BOTTOM: dx = rect_w/2; dy = rect_h; break;
		case COORD_ANCHOR_BOTTOM_RIGHT: dx = rect_w; dy = rect_h; break;
		case COORD_ANCHOR_CENTER:
		default: dx = rect_w/2; dy = rect_h/2; break;
	}
	*out_tl_x = anchor_x - dx;
	*out_tl_y = anchor_y - dy;
}

void coord_resolve_rect(
    coord_length_t pos_x, coord_length_t pos_y,
    coord_length_t size_w, coord_length_t size_h,
    coord_anchor_t anchor, coord_aspect_t aspect,
    int display_w, int display_h,
    int src_w, int src_h,
    coord_rect_t *out)
{
	if (!out) return;

	int anchor_x = pos_x.present
				 ? coord_length_to_px(pos_x, display_w)
				 : display_w / 2;
	int anchor_y = pos_y.present
				 ? coord_length_to_px(pos_y, display_h)
				 : display_h / 2;

	/* The fit/fill math uses this as the constraint. Stretch uses it
	 * literally. Native ignores it. */
	int bound_w = size_w.present
				? coord_length_to_px(size_w, display_w)
				: display_w;
	int bound_h = size_h.present
				? coord_length_to_px(size_h, display_h)
				: display_h;
	if (bound_w < 1) bound_w = 1;
	if (bound_h < 1) bound_h = 1;

	/* Source dims required for aspect math. Fall back to STRETCH if
	 * caller passed zeroes */
	int has_source = (src_w > 0 && src_h > 0);
	coord_aspect_t eff = has_source ? aspect : COORD_ASPECT_STRETCH;

	int actual_w, actual_h;
	switch (eff) {
		case COORD_ASPECT_NATIVE:
			actual_w = src_w;
			actual_h = src_h;
			break;

		case COORD_ASPECT_STRETCH:
			actual_w = bound_w;
			actual_h = bound_h;
			break;

		case COORD_ASPECT_FIT: {
			/* Largest k such that scaled source fits inside bound on
			 * both axes. k = min(bound_w/src_w, bound_h/src_h). */
			double sx = (double)bound_w / (double)src_w;
			double sy = (double)bound_h / (double)src_h;
			double k = (sx < sy) ? sx : sy;
			actual_w = (int)((double)src_w * k + 0.5);
			actual_h = (int)((double)src_h * k + 0.5);
			if (actual_w < 1) actual_w = 1;
			if (actual_h < 1) actual_h = 1;
			break;
		}

		case COORD_ASPECT_FILL: {
			/* Smallest k such that scaled source covers bound on both
			 * axes. k = max(bound_w/src_w, bound_h/src_h). The
			 * resulting rect extends past bound on whatever axis isn't
			 * tight. Clipped at draw. */
			double sx = (double)bound_w / (double)src_w;
			double sy = (double)bound_h / (double)src_h;
			double k = (sx > sy) ? sx : sy;
			actual_w = (int)((double)src_w * k + 0.5);
			actual_h = (int)((double)src_h * k + 0.5);
			if (actual_w < 1) actual_w = 1;
			if (actual_h < 1) actual_h = 1;
			break;
		}

		default:
			actual_w = bound_w;
			actual_h = bound_h;
			break;
	}

	int tl_x, tl_y;
	apply_anchor(anchor, anchor_x, anchor_y, actual_w, actual_h, &tl_x, &tl_y);

	out->x = tl_x;
	out->y = tl_y;
	out->w = actual_w;
	out->h = actual_h;
}
