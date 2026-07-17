// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file renderer.c
 * @brief EGL setup and per-frame scene composite.
 *
 * Brings up EGL, drives the scene composite each frame, and delegates
 * the display connection and render target to a platform vtable. */

#define _POSIX_C_SOURCE 200809L

#include "renderer.h"
#include "renderer_platform.h"
#include "scene.h"
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

static const struct render_platform *pick_platform(enum render_platform_kind sel)
{
	return sel == RENDER_SURFACELESS ? render_platform_surfaceless()
	                                 : render_platform_gbm();
}

static bool renderer_setup_egl(struct renderer *p)
{
	EGLint major, minor;
	if (!eglInitialize(p->egl_dpy, &major, &minor)) {
		fprintf(stderr, "[renderer] eglInitialize failed\n");
		return false;
	}
	DBG("[renderer] EGL %d.%d", major, minor);

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "[renderer] can't bind GLES API\n");
		return false;
	}

	const EGLint cfg_attribs[] = {
		EGL_SURFACE_TYPE, p->plat->surface_type_bit,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLint num_cfg;
	if (!eglChooseConfig(p->egl_dpy, cfg_attribs, &p->egl_cfg, 1, &num_cfg)
		|| num_cfg == 0) {
		fprintf(stderr, "[renderer] no matching EGL config\n");
		return false;
	}

	static const EGLint ctx_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_NONE,
	};
	p->egl_ctx = eglCreateContext(p->egl_dpy, p->egl_cfg, EGL_NO_CONTEXT,
								  ctx_attribs);
	if (p->egl_ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "[renderer] context creation failed: 0x%x\n",
				eglGetError());
		return false;
	}
	return true;
}

bool renderer_init(struct renderer *p, int width, int height,
                   enum render_platform_kind platform)
{
	memset(p, 0, sizeof(*p));
	p->egl_dpy = EGL_NO_DISPLAY;
	p->egl_ctx = EGL_NO_CONTEXT;
	p->egl_surf = EGL_NO_SURFACE;
	p->plat = pick_platform(platform);

	if (!p->plat->display_open(p)) goto fail;
	if (!renderer_setup_egl(p)) goto fail;
	if (!p->plat->target_create(p, width, height)) goto fail;
	if (!p->plat->frame_begin(p)) goto fail;

	p->width = width;
	p->height = height;
	DBG("[renderer] GL_RENDERER: %s", glGetString(GL_RENDERER));
	DBG("[renderer] GL_VERSION:  %s", glGetString(GL_VERSION));

	p->initialized = true;
	DBG("[renderer] init %dx%d", width, height);
	return true;

fail:
	renderer_cleanup(p);
	return false;
}

bool renderer_render(struct renderer *p, struct frame *out)
{
	if (!p->initialized) return false;

	/* Make our target current (a re-compositing output may have left a
	 * different surface current after its blit). Cheap if already current. */
	if (!p->plat->frame_begin(p)) return false;

	/* Composite the scene. `scene_render` dispatches the render phases and
	 * issues `glFinish`, so the present/lock below is safe. */
	scene_render(p->width, p->height);

	if (!p->plat->frame_end(p, out)) return false;
	out->width = p->width;
	out->height = p->height;
	out->serial = ++p->serial;
	return true;
}

bool renderer_resize(struct renderer *p, int width, int height)
{
	if (!p->initialized) return false;
	if (width == p->width && height == p->height) return true;

	p->plat->target_destroy(p);
	if (!p->plat->target_create(p, width, height)) {
		fprintf(stderr, "[renderer] resize: target_create %dx%d failed\n",
				width, height);
		return false;
	}
	if (!p->plat->frame_begin(p)) return false;

	p->width = width;
	p->height = height;
	DBG("[renderer] resized to %dx%d", width, height);
	return true;
}

void renderer_cleanup(struct renderer *p)
{
	if (!p) return;

	if (p->plat && p->egl_dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(p->egl_dpy, p->egl_surf, p->egl_surf, p->egl_ctx);
		p->plat->target_destroy(p);

		eglMakeCurrent(p->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (p->egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(p->egl_dpy, p->egl_ctx);
		eglTerminate(p->egl_dpy);
	}
	p->egl_surf = EGL_NO_SURFACE;
	p->egl_ctx = EGL_NO_CONTEXT;
	p->egl_dpy = EGL_NO_DISPLAY;

	if (p->plat) p->plat->display_close(p);
	p->plat = NULL;
	p->initialized = false;
}
