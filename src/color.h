// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file color.h
 * @brief Hex color parse and format. */

#ifndef COLOR_H
#define COLOR_H

#ifdef __cplusplus
extern "C" {
#endif

/** Parse 8-hex RRGGBBAA or 6-hex RRGGBB. Alpha defaults to ff.
 * Colors are four floats. Leading '#' tolerated. */
int color_parse_hex(const char *s, float *out_r, float *out_g,
                    float *out_b, float *out_a);

/** Format as 8-hex RRGGBBAA string. No '#'. `out` must hold at least
 * 9 bytes. Always NULL-terminates. */
void color_to_hex(float r, float g, float b, float a, char *out);

#ifdef __cplusplus
}
#endif

#endif
