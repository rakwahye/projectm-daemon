// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file filter.h
 * @brief Color + alpha filter.
 *
 * A fullscreen alpha-blended quad drawn over the finished frame. Alpha
 * zero is off. */

#ifndef FILTER_H
#define FILTER_H

#ifdef __cplusplus
extern "C" {
#endif

struct filter_config {
	float r, g, b, a;
};

/** Apply the config slice, so callers do not push the color in at bringup. */
int filter_init(void);

/** Queue a filter change. Thread-safe. Alpha 0 = off. */
void filter_set(float r, float g, float b, float a);

/** Re-apply the config slice. */
void filter_config_apply(void);

/** Draw the filter quad into the currently-bound framebuffer. */
void filter_render(void);

void filter_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
