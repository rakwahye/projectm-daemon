// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file renderer.h
 * @brief Single shared scene renderer.
 *
 * Renders the scene once per frame into an off-screen surface. The
 * result is exposed to N outputs to scan out or re-composite. Owns
 * the only EGL context. */

#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Held front-buffer depth. 3 gives a frame of margin over the
 * vblank-bound minimum of 2, tolerating an output that skips a frame. */
#define RENDERER_BO_RING 3

/** Render platform selector. GBM opens a render node and scans out real
 * buffers. SURFACELESS uses an offscreen FBO with no device, for
 * headless-only runs (CI, no GPU). */
enum render_platform_kind {
	RENDER_GBM,
	RENDER_SURFACELESS,
};

struct render_platform;

/** One rendered frame, valid between `renderer_render` and the next
 * call. Outputs must finish using `scanout_handle` and `gl_tex` before
 * the next render cycle. */
struct frame {
	void *scanout_handle; // platform-owned; only the scanout output knows the type
	GLuint gl_tex; // texture view of the frame, for re-composite
	int width;
	int height;
	unsigned serial; // increments each render
};

struct renderer {
	/** EGL, owned here, borrowed by re-compositing outputs. */
	EGLDisplay egl_dpy;
	EGLConfig egl_cfg;
	EGLContext egl_ctx;
	EGLSurface egl_surf;

	int width;
	int height;
	unsigned serial;

	/** Platform vtable and its private state (GBM device, surface, and
	 * ring, or the surfaceless FBO). Shared code never looks inside. */
	const struct render_platform *plat;
	void *plat_state;

	bool initialized;
};

/** Create EGL and the render target on the given platform at width x
 * height and make current. Partial state is cleaned up on failure.
 * @returns true on success. */
bool renderer_init(struct renderer *p, int width, int height,
                   enum render_platform_kind platform);

/** Composite one frame into the offscreen surface and fill `*out`.
 * @returns true on success, false on a dropped frame. */
bool renderer_render(struct renderer *p, struct frame *out);

/** Rebuild the offscreen surface at new dimensions. Tears down the cache. */
bool renderer_resize(struct renderer *p, int width, int height);

void renderer_cleanup(struct renderer *p);

#ifdef __cplusplus
}
#endif

#endif
