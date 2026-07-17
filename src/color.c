// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file color.c
 * @brief Hex color parse and format. */

#define _POSIX_C_SOURCE 200809L

#include "color.h"
#include <string.h>

static int parse_hex_n(const char *s, int count, unsigned *out) {
	unsigned v = 0;
	for (int i = 0; i < count; i++) {
		char c = s[i];
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		v = (v << 4) | (unsigned)d;
	}
	*out = v;
	return 1;
}

int color_parse_hex(const char *s, float *out_r, float *out_g, float *out_b,
                    float *out_a) {
	if (!s) return 0;

	if (s[0] == '#') s++;

	size_t len = strlen(s);
	if (len != 6 && len != 8) return 0;

	unsigned r, g, b, a = 0xff;
	if (!parse_hex_n(s + 0, 2, &r)) return 0;
	if (!parse_hex_n(s + 2, 2, &g)) return 0;
	if (!parse_hex_n(s + 4, 2, &b)) return 0;
	if (len == 8) {
		if (!parse_hex_n(s + 6, 2, &a)) return 0;
	}

	*out_r = (float)r / 255.0f;
	*out_g = (float)g / 255.0f;
	*out_b = (float)b / 255.0f;
	*out_a = (float)a / 255.0f;
	return 1;
}

void color_to_hex(float r, float g, float b, float a, char *out) {
	int ri = (int)(r * 255.0f + 0.5f);
	int gi = (int)(g * 255.0f + 0.5f);
	int bi = (int)(b * 255.0f + 0.5f);
	int ai = (int)(a * 255.0f + 0.5f);
	if (ri < 0) ri = 0;
	if (ri > 255) ri = 255;
	if (gi < 0) gi = 0;
	if (gi > 255) gi = 255;
	if (bi < 0) bi = 0;
	if (bi > 255) bi = 255;
	if (ai < 0) ai = 0;
	if (ai > 255) ai = 255;
	static const char hx[] = "0123456789abcdef";
	out[0] = hx[(ri >> 4) & 0xf]; out[1] = hx[ri & 0xf];
	out[2] = hx[(gi >> 4) & 0xf]; out[3] = hx[gi & 0xf];
	out[4] = hx[(bi >> 4) & 0xf]; out[5] = hx[bi & 0xf];
	out[6] = hx[(ai >> 4) & 0xf]; out[7] = hx[ai & 0xf];
	out[8] = '\0';
}
