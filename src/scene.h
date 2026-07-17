// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file scene.h
 * @brief Per-frame GL compositor.
 *
 * Composites one frame into the bound target: background, engine
 * output, overlays, and the render-phase modules. */

#ifndef SCENE_H
#define SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

/** Single RRGGBBAA token. Four [0,1] floats. */
struct scene_config {
	float bg_r, bg_g, bg_b, bg_alpha;
};

/** Composite one frame at width * height. Ends with glFinish. */
void scene_render(int width, int height);

#ifdef __cplusplus
}
#endif

#endif
