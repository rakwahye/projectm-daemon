// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file renderer_gbm.c
 * @brief GBM render platform.
 *
 * Opens a DRM render node, renders into a `gbm_surface`, and hands
 * out locked front buffers for direct scanout plus an EGLImage
 * texture view for re-compositing outputs. */

#define _POSIX_C_SOURCE 200809L

#include "renderer_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

struct gbm_teximg {
	struct gbm_bo *bo;
	EGLImageKHR image;
	GLuint tex;
	struct gbm_teximg *next;
};

struct gbm_state {
	int drm_fd;
	struct gbm_device *dev;
	struct gbm_surface *surf;
	struct gbm_teximg *tex_cache;
	struct gbm_bo *held_bos[RENDERER_BO_RING];
	int n_held;
};

static struct gbm_surface *gbm_surf_create_with_fallback(
	struct gbm_device *dev, int w, int h)
{
	struct gbm_surface *s = gbm_surface_create(dev, w, h,
		GBM_FORMAT_XRGB8888,
		GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (s) return s;
	return gbm_surface_create(dev, w, h,
		GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
}

/* Import a bo as a GL texture via dmabuf/EGLImage, caching on first sight. */
static struct gbm_teximg *tex_for_bo(struct renderer *p, struct gbm_state *g,
                                     struct gbm_bo *bo)
{
	for (struct gbm_teximg *t = g->tex_cache; t; t = t->next)
		if (t->bo == bo) return t;

	int fd = gbm_bo_get_fd(bo);
	uint32_t width = gbm_bo_get_width(bo);
	uint32_t height = gbm_bo_get_height(bo);
	uint32_t stride = gbm_bo_get_stride(bo);
	uint32_t format = gbm_bo_get_format(bo);
	if (fd < 0) {
		fprintf(stderr, "[renderer] gbm_bo_get_fd failed\n");
		return NULL;
	}

	EGLint attrs[] = {
		EGL_WIDTH, (EGLint)width,
		EGL_HEIGHT, (EGLint)height,
		EGL_LINUX_DRM_FOURCC_EXT, (EGLint)format,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
		EGL_NONE,
	};

	EGLImageKHR img = p_eglCreateImageKHR(p->egl_dpy, EGL_NO_CONTEXT,
		EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	close(fd); // eglCreateImageKHR dups internally
	if (img == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "[renderer] eglCreateImageKHR failed: 0x%x\n",
				eglGetError());
		return NULL;
	}

	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
	glBindTexture(GL_TEXTURE_2D, 0);

	struct gbm_teximg *node = calloc(1, sizeof(*node));
	if (!node) {
		glDeleteTextures(1, &tex);
		p_eglDestroyImageKHR(p->egl_dpy, img);
		return NULL;
	}
	node->bo = bo;
	node->image = img;
	node->tex = tex;
	node->next = g->tex_cache;
	g->tex_cache = node;

	DBG("[renderer] imported bo %p -> tex %u (%ux%u)", (void *)bo, tex,
		width, height);
	return node;
}

static void tex_cache_clear(struct renderer *p, struct gbm_state *g)
{
	struct gbm_teximg *t = g->tex_cache;
	while (t) {
		struct gbm_teximg *next = t->next;
		if (t->tex) glDeleteTextures(1, &t->tex);
		if (t->image) p_eglDestroyImageKHR(p->egl_dpy, t->image);
		free(t);
		t = next;
	}
	g->tex_cache = NULL;
}

static void drain_ring(struct gbm_state *g)
{
	for (int i = 0; i < g->n_held; i++)
		gbm_surface_release_buffer(g->surf, g->held_bos[i]);
	g->n_held = 0;
}

static bool gbm_display_open(struct renderer *p)
{
	struct gbm_state *g = calloc(1, sizeof(*g));
	if (!g) return false;
	g->drm_fd = -1;
	p->plat_state = g;

	const char *render_nodes[] = {
		"/dev/dri/renderD128", "/dev/dri/renderD129", NULL
	};
	for (int i = 0; render_nodes[i]; i++) {
		g->drm_fd = open(render_nodes[i], O_RDWR | O_CLOEXEC);
		if (g->drm_fd >= 0) {
			DBG("[renderer] opened %s (fd=%d)", render_nodes[i], g->drm_fd);
			break;
		}
	}
	if (g->drm_fd < 0) {
		fprintf(stderr, "[renderer] cannot open any render node: %s\n",
				strerror(errno));
		return false;
	}

	g->dev = gbm_create_device(g->drm_fd);
	if (!g->dev) {
		fprintf(stderr, "[renderer] gbm_create_device failed\n");
		return false;
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (eglGetPlatformDisplayEXT) {
		p->egl_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, g->dev, NULL);
	} else {
		p->egl_dpy = eglGetDisplay((EGLNativeDisplayType)g->dev);
	}
	if (p->egl_dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "[renderer] no EGL display\n");
		return false;
	}
	return true;
}

static bool gbm_target_create(struct renderer *p, int w, int h)
{
	struct gbm_state *g = p->plat_state;

	g->surf = gbm_surf_create_with_fallback(g->dev, w, h);
	if (!g->surf) {
		fprintf(stderr, "[renderer] gbm_surface_create %dx%d failed\n", w, h);
		return false;
	}

	p->egl_surf = eglCreateWindowSurface(p->egl_dpy, p->egl_cfg,
		(EGLNativeWindowType)g->surf, NULL);
	if (p->egl_surf == EGL_NO_SURFACE) {
		fprintf(stderr, "[renderer] eglCreateWindowSurface failed: 0x%x\n",
				eglGetError());
		return false;
	}

	/* Resolve EGLImage import entrypoints (needed for the tex cache). */
	if (!p_eglCreateImageKHR) {
		p_eglCreateImageKHR =
			(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		p_eglDestroyImageKHR =
			(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		p_glEGLImageTargetTexture2DOES =
			(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
			eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if (!p_eglCreateImageKHR || !p_eglDestroyImageKHR ||
			!p_glEGLImageTargetTexture2DOES) {
			fprintf(stderr,
				"[renderer] EGLImage import extensions unavailable "
				"(need EGL_KHR_image_base + EGL_EXT_image_dma_buf_import + "
				"GL_OES_EGL_image)\n");
			return false;
		}
	}
	return true;
}

static void gbm_target_destroy(struct renderer *p)
{
	struct gbm_state *g = p->plat_state;
	if (!g) return;

	if (g->surf) drain_ring(g);
	tex_cache_clear(p, g);

	if (p->egl_surf != EGL_NO_SURFACE) {
		eglDestroySurface(p->egl_dpy, p->egl_surf);
		p->egl_surf = EGL_NO_SURFACE;
	}
	if (g->surf) { gbm_surface_destroy(g->surf); g->surf = NULL; }
}

static bool gbm_frame_begin(struct renderer *p)
{
	if (!eglMakeCurrent(p->egl_dpy, p->egl_surf, p->egl_surf, p->egl_ctx)) {
		fprintf(stderr, "[renderer] make current failed: 0x%x\n",
				eglGetError());
		return false;
	}
	return true;
}

static bool gbm_frame_end(struct renderer *p, struct frame *out)
{
	struct gbm_state *g = p->plat_state;

	if (!eglSwapBuffers(p->egl_dpy, p->egl_surf)) {
		fprintf(stderr, "[renderer] eglSwapBuffers failed: 0x%x\n",
				eglGetError());
		return false;
	}

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(g->surf);
	if (!bo) {
		fprintf(stderr, "[renderer] lock_front_buffer failed\n");
		return false;
	}

	struct gbm_teximg *t = tex_for_bo(p, g, bo);
	if (!t) {
		gbm_surface_release_buffer(g->surf, bo);
		return false;
	}

	if (g->n_held >= RENDERER_BO_RING) {
		gbm_surface_release_buffer(g->surf, g->held_bos[0]);
		memmove(&g->held_bos[0], &g->held_bos[1],
				(size_t)(g->n_held - 1) * sizeof(g->held_bos[0]));
		g->n_held--;
	}
	g->held_bos[g->n_held++] = bo;

	out->scanout_handle = bo;
	out->gl_tex = t->tex;
	return true;
}

static void gbm_display_close(struct renderer *p)
{
	struct gbm_state *g = p->plat_state;
	if (!g) return;
	if (g->dev) gbm_device_destroy(g->dev);
	if (g->drm_fd >= 0) close(g->drm_fd);
	free(g);
	p->plat_state = NULL;
}

static const struct render_platform gbm_platform = {
	.surface_type_bit = EGL_WINDOW_BIT,
	.display_open = gbm_display_open,
	.target_create = gbm_target_create,
	.target_destroy = gbm_target_destroy,
	.frame_begin = gbm_frame_begin,
	.frame_end = gbm_frame_end,
	.display_close = gbm_display_close,
};

const struct render_platform *render_platform_gbm(void)
{
	return &gbm_platform;
}

struct gbm_device *renderer_gbm_device(struct renderer *p)
{
	if (!p->plat || p->plat != &gbm_platform) return NULL;
	struct gbm_state *g = p->plat_state;
	return g ? g->dev : NULL;
}
