// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file renderer_platform.h
 * @brief Render platform vtable.
 *
 * Each platform supplies the display connection and render target
 * the renderer draws into. Its state stays private to the platform.
 * GBM uses a render node and scans out real buffers. Surfaceless
 * uses an offscreen FBO with no device, for headless runs. */

#ifndef RENDERER_PLATFORM_H
#define RENDERER_PLATFORM_H

#include "renderer.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct render_platform {
	/** EGL_SURFACE_TYPE the config must support. EGL_WINDOW_BIT for GBM,
	 * EGL_PBUFFER_BIT for surfaceless FBO rendering. */
	int surface_type_bit;

	/** Open the display connection and pick the EGLDisplay. GBM opens a
	 * render node and `gbm_device`. Surfaceless takes the default display.
	 * Fills `p->egl_dpy`. */
	bool (*display_open)(struct renderer *p);

	/** Create the render target at w x h and the matching EGLSurface (or
	 * none, for FBO platforms). Called after the context exists. */
	bool (*target_create)(struct renderer *p, int w, int h);

	/** Destroy just the target, for the resize path. */
	void (*target_destroy)(struct renderer *p);

	/** Make the render target current and ready to draw. */
	bool (*frame_begin)(struct renderer *p);

	/** Finish the frame: present or lock, then fill `*out`.
	 * @returns false to drop the frame. */
	bool (*frame_end)(struct renderer *p, struct frame *out);

	/** Release everything `display_open` and `target_create` acquired. */
	void (*display_close)(struct renderer *p);
};

const struct render_platform *render_platform_gbm(void);
const struct render_platform *render_platform_surfaceless(void);

struct gbm_device;
/** The GBM device the renderer draws on, for outputs that create their
 * own recompositing surface. @returns NULL when surfaceless. */
struct gbm_device *renderer_gbm_device(struct renderer *p);

#ifdef __cplusplus
}
#endif

#endif
