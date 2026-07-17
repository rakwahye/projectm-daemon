// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file scene_router.h
 * @brief Deposit routing for present-phase image delivery.
 *
 * A renderer produces an image blind to where it lands. If the engine
 * advertises `deposit`, deliver into its own canvas with its blend
 * semantics. If not, composite onto the scene as an alpha-blended
 * quad. */

#ifndef SCENE_ROUTER_H
#define SCENE_ROUTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct visualizer;

/** Deliver a texture at its destination rect.
 * `surf_w` and `surf_h` size the scene surface. The quad path maps the
 * rect into them, the canvas path ignores them. `alpha` is applied by
 * the quad path only, since the canvas path has strength baked into the
 * texture already. */
 void scene_router_deposit(struct visualizer *vis, uint32_t tex, int x, int y,
                           int w, int h, int surf_w, int surf_h, float alpha);

#ifdef __cplusplus
}
#endif

#endif
