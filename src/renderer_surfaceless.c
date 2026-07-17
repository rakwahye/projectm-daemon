// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file renderer_surfaceless.c
 * @brief Surfaceless render platform.
 *
 * Renders into an offscreen FBO with no device, for headless runs
 * on a software driver. Nothing samples the frame, so there is a
 * single FBO, no buffer ring, and `scanout_handle` is always NULL. */

#define _POSIX_C_SOURCE 200809L

#include "renderer_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

struct sl_state {
	GLuint fbo;
	GLuint tex;
};

static bool sl_display_open(struct renderer *p)
{
	struct sl_state *s = calloc(1, sizeof(*s));
	if (!s) return false;
	p->plat_state = s;

	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (getPlatformDisplay) {
		p->egl_dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
		                                EGL_DEFAULT_DISPLAY, NULL);
	} else {
		p->egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}
	if (p->egl_dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "[renderer] no surfaceless EGL display\n");
		return false;
	}
	return true;
}

static void sl_target_destroy(struct renderer *p)
{
	struct sl_state *s = p->plat_state;
	if (!s) return;
	if (s->fbo) { glDeleteFramebuffers(1, &s->fbo); s->fbo = 0; }
	if (s->tex) { glDeleteTextures(1, &s->tex); s->tex = 0; }
}

static bool sl_target_create(struct renderer *p, int w, int h)
{
	struct sl_state *s = p->plat_state;

	/* Context has no surface; bind it with no draw/read surface so we can
	 * build the FBO. */
	if (!eglMakeCurrent(p->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, p->egl_ctx)) {
		fprintf(stderr, "[renderer] surfaceless make current failed: 0x%x\n",
				eglGetError());
		return false;
	}

	glGenTextures(1, &s->tex);
	glBindTexture(GL_TEXTURE_2D, s->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &s->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, s->tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "[renderer] surfaceless FBO incomplete\n");
		return false;
	}
	DBG("[renderer] surfaceless FBO %dx%d", w, h);
	return true;
}

static bool sl_frame_begin(struct renderer *p)
{
	struct sl_state *s = p->plat_state;
	if (!eglMakeCurrent(p->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, p->egl_ctx)) {
		fprintf(stderr, "[renderer] surfaceless make current failed: 0x%x\n",
				eglGetError());
		return false;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
	return true;
}

static bool sl_frame_end(struct renderer *p, struct frame *out)
{
	struct sl_state *s = p->plat_state;
	glFinish();
	out->scanout_handle = NULL;
	out->gl_tex = s->tex;
	return true;
}

static void sl_display_close(struct renderer *p)
{
	struct sl_state *s = p->plat_state;
	if (!s) return;
	free(s);
	p->plat_state = NULL;
}

static const struct render_platform sl_platform = {
	.surface_type_bit = EGL_PBUFFER_BIT,
	.display_open = sl_display_open,
	.target_create = sl_target_create,
	.target_destroy = sl_target_destroy,
	.frame_begin = sl_frame_begin,
	.frame_end = sl_frame_end,
	.display_close = sl_display_close,
};

const struct render_platform *render_platform_surfaceless(void)
{
	return &sl_platform;
}
