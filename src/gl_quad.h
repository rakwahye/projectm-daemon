// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file gl_quad.h
 * @brief Fullscreen and positioned quad drawing.
 *
 * VAO-only quads with alpha blend, positions from `gl_VertexID`.
 * Callers pass straight RGBA. Premultiply happens internally. */

#ifndef GL_QUAD_H
#define GL_QUAD_H

#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

int gl_quad_init(void);
void gl_quad_destroy(void);

/** Draw a fullscreen quad with a constant color, alpha-blended over
 * the destination. Color is premultiplied internally (we expect
 * straight RGBA from the caller). */
void gl_quad_tint(float r, float g, float b, float a);

/** Fill alpha-holes with bg color */
void gl_quad_fill_holes(float r, float g, float b, float a);

/** Draw a textured quad at (x,y,w,h) in pixels. tex must contain
 * premultiplied RGBA. alpha modulates the entire fragment. */
void gl_quad_blit(GLuint tex, int x, int y, int w, int h, int surf_w,
                  int surf_h, float alpha);

#ifdef __cplusplus
}
#endif

#endif
