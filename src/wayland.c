// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wayland.c
 * @brief Wayland output.
 *
 * Connects to the compositor, creates a surface in its shell role
 * (wallpaper or window), and re-composites each frame into it. Exposes
 * the bound globals for front-layer modules. */

#define _POSIX_C_SOURCE 200809L

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "output.h"
#include "renderer.h"
#include "renderer_platform.h"
#include "gl_quad.h"
#include "app_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <wayland-client.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

typedef enum {
	WL_ROLE_AUTO = 0,
	WL_ROLE_WALLPAPER,
	WL_ROLE_WINDOWED,
} wl_role_t;

extern int g_debug;
extern _Atomic sig_atomic_t g_running;
#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

#define NUM_BUFFERS 3

struct wl_priv; // fwd. frame_buffer carries a back-pointer
struct retiring_surface; // fwd. old surfaces awaiting buffer release

struct frame_buffer {
	struct wl_buffer *wl_buf;
	struct gbm_bo *bo;
	struct gbm_surface *owner; // surface this bo was locked from
	struct wl_priv *priv; // back-pointer for buffer release
	bool busy;
};

struct wl_priv {
	struct renderer *prod;
	struct gbm_device *gbm_dev; // == prod->gbm_dev
	EGLDisplay egl_dpy; // == prod->egl_dpy
	EGLConfig egl_cfg; // == prod->egl_cfg
	EGLContext egl_ctx; // == prod->egl_ctx

	struct gbm_surface *gbm_surf;
	EGLSurface egl_surf;

	struct wl_display *wl_dpy;
	struct wl_registry *wl_reg;
	struct wl_compositor *wl_comp;
	struct wl_shm *wl_shm;
	struct wl_output *wl_out;
	struct wl_surface *wl_surf;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct xdg_wm_base *xdg_shell;
	struct xdg_surface *xdg_surf;
	struct xdg_toplevel *xdg_top;
	struct zwp_linux_dmabuf_v1 *dmabuf_iface;

	int output_width, output_height, output_refresh_hz;
	int pending_w, pending_h;
	bool configured_received;
	wl_role_t sub_mode; // WALLPAPER or WINDOWED

	struct wl_callback *frame_cb;
	bool frame_scheduled;
	int wl_due;

	struct frame_buffer frame_bufs[NUM_BUFFERS];
	int current_buf;

	/* Old, resized-away gbm/EGL surfaces kept alive until the compositor
	 * releases every buffer still backed by them. */
	struct retiring_surface *retiring;

	/* Last rendered frame's texture, cached so the configure handler can
	 * re-blit synchronously on resize. */
	GLuint last_tex;
	int last_master_w, last_master_h;
};

static bool output_wayland_apply_resize(struct wl_priv *p);
static bool wl_blit_and_commit(struct wl_priv *p, GLuint scene_tex,
                               bool schedule_callback);

static void output_mode(void *data, struct wl_output *out,
                        uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	(void)out;
	struct wl_priv *p = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		p->output_width = w;
		p->output_height = h;
		p->output_refresh_hz = (refresh + 500) / 1000; // mHz -> Hz
		DBG("[wl-output] mode: %dx%d@%dHz", w, h, p->output_refresh_hz);
	}
}
static void output_geometry(void *d, struct wl_output *o, int32_t x,
                            int32_t y, int32_t pw, int32_t ph,
                            int32_t sp, const char *mk, const char *md,
                            int32_t tr)
{
	(void)d; (void)o; (void)x; (void)y; (void)pw;
	(void)ph; (void)sp; (void)mk; (void)md; (void)tr;
}
static void output_done(void *d, struct wl_output *o) {
	(void)d;(void)o;
}
static void output_scale(void *d, struct wl_output *o, int32_t f) {
	(void)d; (void)o; (void)f;
}
static void output_name(void *d, struct wl_output *o, const char *n) {
	(void)d; (void)o; (void)n;
}
static void output_desc(void *d, struct wl_output *o, const char *s) {
	(void)d; (void)o; (void)s;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry, .mode = output_mode, .done = output_done,
	.scale = output_scale, .name = output_name, .description = output_desc,
};

/* Old surface stays alive until all busy buffers are released */
struct retiring_surface {
	struct gbm_surface *gbm_surf;
	EGLSurface egl_surf;
	int outstanding; // Busy buffers still referencing it
	struct retiring_surface *next;
};

/* A buffer backed by a retired surface was just released. Drop the
 * surface's outstanding count and destroy it once nothing references
 * it any more. */
static void retiring_release_one(struct wl_priv *p, struct gbm_surface *surf) {
	struct retiring_surface **pp = &p->retiring;
	while (*pp) {
		struct retiring_surface *r = *pp;
		if (r->gbm_surf == surf) {
			if (--r->outstanding <= 0) {
				if (p->egl_dpy != EGL_NO_DISPLAY && r->egl_surf
				    != EGL_NO_SURFACE) {
					eglDestroySurface(p->egl_dpy, r->egl_surf);
				}
				gbm_surface_destroy(r->gbm_surf);
				*pp = r->next;
				free(r);
			}
			return;
		}
		pp = &r->next;
	}
}

static void buffer_release(void *data, struct wl_buffer *buf) {
	(void)buf;
	struct frame_buffer *fb = data;
	/* The compositor is done with this buffer. Destroy the wl_buffer and
	 * return its BO to the surface it was locked from (fb->owner - which may
	 * be the live surface or a retired one). Releasing here (rather than
	 * deferring to slot reuse) keeps non-busy ring slots empty, which is what
	 * lets a resize leave busy slots untouched and reuse only the free ones. */
	fb->busy = false;
	if (fb->wl_buf) { wl_buffer_destroy(fb->wl_buf); fb->wl_buf = NULL; }
	if (fb->bo && fb->owner) {
		gbm_surface_release_buffer(fb->owner, fb->bo);
		fb->bo = NULL;
	}
	/* If the BO belonged to a retired surface, that surface is
	 * kept alive until its last in-flight buffer is released - now. */
	if (fb->priv && fb->owner && fb->owner != fb->priv->gbm_surf)
		retiring_release_one(fb->priv, fb->owner);
	fb->owner = NULL;
}
static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *s,
                            uint32_t serial, uint32_t w, uint32_t h)
{
	struct wl_priv *p = data;
	DBG("[wl-output] layer configure: %ux%u", w, h);
	zwlr_layer_surface_v1_ack_configure(s, serial);
	if (w && h) {
		if (!p->configured_received) {
			/* First configure. Set the working size directly. */
			p->output_width = w; p->output_height = h;
		} else {
			/* Resize. Record as pending. */
			p->pending_w = w; p->pending_h = h;
		}
	}
	if (!p->configured_received) p->configured_received = true;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *s) {
	(void)data; (void)s;
	g_running = 0;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure, .closed = layer_closed,
};

/* xdg_toplevel.configure carries the size and states the compositor
 * wants. Per spec it fires BEFORE the matching xdg_surface.configure,
 * so we just record the requested size here. xdg_surf_configure acks
 * and applies it synchronously. */
static void xdg_top_configure(void *data, struct xdg_toplevel *t,
	int32_t w, int32_t h, struct wl_array *states)
{
	(void)t; (void)states;
	struct wl_priv *p = data;
	DBG("[wl-output] xdg-top configure: %dx%d", w, h);
	if (w && h) { p->pending_w = w; p->pending_h = h; }
}
static void xdg_top_close(void *data, struct xdg_toplevel *t) {
	(void)data; (void)t;
	g_running = 0;
}
static const struct xdg_toplevel_listener xdg_top_listener_impl = {
	.configure = xdg_top_configure, .close = xdg_top_close,
};
static void xdg_surf_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	struct wl_priv *p = data;
	xdg_surface_ack_configure(xs, serial);

	if (!p->configured_received) {
		/* First configure. Init finishes building the surfaces and the
		 * first frame after this returns. */
		if (p->pending_w && p->pending_h) {
			p->output_width = p->pending_w;
			p->output_height = p->pending_h;
			p->pending_w = p->pending_h = 0;
		}
		p->configured_received = true;
		return;
	}

	/* Synchronous resize. Do NOT defer. */
	if (p->pending_w && p->pending_h &&
		(p->pending_w != p->output_width || p->pending_h != p->output_height)) {
		if (!output_wayland_apply_resize(p)) { g_running = 0; return; }
		if (p->last_tex)
			wl_blit_and_commit(p, p->last_tex, false);
	} else {
		/* State-only change with no size delta. Clear any pending size,
		 * and do nothing. */
		p->pending_w = p->pending_h = 0;
	}
}
static const struct xdg_surface_listener xdg_surf_listener_impl = {
	.configure = xdg_surf_configure,
};
static void xdg_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	(void)data; xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener xdg_wm_base_listener_impl = {
	.ping = xdg_ping,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { .done = frame_done };
static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	(void)time;
	struct wl_priv *p = data;
	wl_callback_destroy(cb);
	p->frame_cb = NULL;
	p->frame_scheduled = false;
	p->wl_due = 1;
}

static struct wl_priv *g_wl_singleton = NULL;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
	const char *iface, uint32_t version)
{
	struct wl_priv *p = data;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		p->wl_comp = wl_registry_bind(reg, name, &wl_compositor_interface,
			version < 4 ? version : 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		p->wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		p->wl_out = wl_registry_bind(reg, name, &wl_output_interface,
			version < 4 ? version : 4);
		wl_output_add_listener(p->wl_out, &output_listener, p);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		p->layer_shell = wl_registry_bind(reg, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		p->xdg_shell = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(p->xdg_shell, &xdg_wm_base_listener_impl, p);
	} else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
		p->dmabuf_iface = wl_registry_bind(reg, name,
			&zwp_linux_dmabuf_v1_interface, version < 3 ? version : 3);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {
	(void)d;(void)r;(void)n;
}

static const struct wl_registry_listener registry_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

static struct gbm_surface *gbm_surf_create_with_fallback(
	struct gbm_device *dev, int w, int h)
{
	struct gbm_surface *s = gbm_surface_create(dev, w, h,
		GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (s) return s;
	return gbm_surface_create(dev, w, h, GBM_FORMAT_XRGB8888,
		GBM_BO_USE_RENDERING);
}

static bool output_wayland_init(struct output *c, struct renderer *prod)
{
	struct wl_priv *p = c->priv;

	/* Borrow the renderer's GBM device and EGL. */
	p->prod = prod;
	p->gbm_dev = renderer_gbm_device(prod);
	p->egl_dpy = prod->egl_dpy;
	p->egl_cfg = prod->egl_cfg;
	p->egl_ctx = prod->egl_ctx;
	p->wl_due = 1;

	p->wl_dpy = wl_display_connect(NULL);
	if (!p->wl_dpy) { LOG("[wl-output] cannot connect to Wayland"); return false; }

	p->wl_reg = wl_display_get_registry(p->wl_dpy);
	wl_registry_add_listener(p->wl_reg, &registry_listener, p);
	wl_display_roundtrip(p->wl_dpy); // Globals
	wl_display_roundtrip(p->wl_dpy); // Output modes

	if (!p->wl_comp) { LOG("[wl-output] no wl_compositor"); return false; }
	if (!p->dmabuf_iface) { LOG("[wl-output] no zwp_linux_dmabuf_v1"); return false; }

	if (p->sub_mode == WL_ROLE_AUTO) {
		if (p->layer_shell) p->sub_mode = WL_ROLE_WALLPAPER;
		else if (p->xdg_shell) p->sub_mode = WL_ROLE_WINDOWED;
		else { LOG("[wl-output] no layer-shell or xdg-shell"); return false; }
	} else if (p->sub_mode == WL_ROLE_WALLPAPER && !p->layer_shell) {
		LOG("[wl-output] wallpaper requested but no wlr-layer-shell"); return false;
	} else if (p->sub_mode == WL_ROLE_WINDOWED && !p->xdg_shell) {
		LOG("[wl-output] windowed requested but no xdg-shell"); return false;
	}

	/* In WINDOWED mode the daemon owns its own initial dimensions. Override
	 * to a modest default so the WM can tile/resize freely. */
	if (p->sub_mode == WL_ROLE_WINDOWED) {
		p->output_width = 1280;
		p->output_height = 720;
	}

	p->wl_surf = wl_compositor_create_surface(p->wl_comp);
	if (p->sub_mode == WL_ROLE_WALLPAPER) {
		p->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			p->layer_shell, p->wl_surf, p->wl_out,
			ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, app_paths_app_name());
		zwlr_layer_surface_v1_set_anchor(p->layer_surface,
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
		zwlr_layer_surface_v1_set_exclusive_zone(p->layer_surface, -1);
		zwlr_layer_surface_v1_set_size(p->layer_surface, 0, 0);
		zwlr_layer_surface_v1_add_listener(p->layer_surface, &layer_listener, p);
	} else {
		p->xdg_surf = xdg_wm_base_get_xdg_surface(p->xdg_shell, p->wl_surf);
		xdg_surface_add_listener(p->xdg_surf, &xdg_surf_listener_impl, p);
		p->xdg_top = xdg_surface_get_toplevel(p->xdg_surf);
		xdg_toplevel_add_listener(p->xdg_top, &xdg_top_listener_impl, p);
		xdg_toplevel_set_title(p->xdg_top, app_paths_app_name());
		xdg_toplevel_set_app_id(p->xdg_top, app_paths_app_name());
	}
	wl_surface_commit(p->wl_surf);

	while (!p->configured_received && g_running) {
		if (wl_display_dispatch(p->wl_dpy) < 0) {
			LOG("[wl-output] dispatch failed waiting for configure: %s",
				strerror(errno));
			return false;
		}
	}
	if (!p->configured_received) return false;

	/* Our own blit-target surface, on the renderer's gbm device, and an
	 * EGL window surface over it against the renderer's context. */
	p->gbm_surf = gbm_surf_create_with_fallback(p->gbm_dev,
		p->output_width, p->output_height);
	if (!p->gbm_surf) { LOG("[wl-output] gbm_surface_create failed"); return false; }

	p->egl_surf = eglCreateWindowSurface(p->egl_dpy, p->egl_cfg,
		(EGLNativeWindowType)p->gbm_surf, NULL);
	if (p->egl_surf == EGL_NO_SURFACE) {
		LOG("[wl-output] eglCreateWindowSurface failed: 0x%x", eglGetError());
		return false;
	}

	memset(p->frame_bufs, 0, sizeof(p->frame_bufs));
	p->current_buf = 0;

	DBG("[wl-output] init complete: %dx%d, %s", p->output_width,
		p->output_height,
		p->sub_mode == WL_ROLE_WALLPAPER ? "wallpaper" : "windowed");

	/* Expose this output's globals to overlay FRONT-mode init via
	 * the wayland_get_* accessors. */
	g_wl_singleton = p;
	return true;
}

static bool output_wayland_apply_resize(struct wl_priv *p)
{
	int nw = p->pending_w, nh = p->pending_h;
	p->pending_w = p->pending_h = 0;
	if (nw <= 0 || nh <= 0) return true;
	if (nw == p->output_width && nh == p->output_height) return true;

	DBG("[wl-output] resize %dx%d -> %dx%d",
		p->output_width, p->output_height, nw, nh);

	if (p->frame_cb) { wl_callback_destroy(p->frame_cb); p->frame_cb = NULL; }
	p->frame_scheduled = false;

	/* Retire the old surface. The old gbm/EGL surface stays alive
	 * until the count reaches zero. */
	int still_busy = 0;
	for (int i = 0; i < NUM_BUFFERS; i++)
		if (p->frame_bufs[i].busy && p->frame_bufs[i].owner == p->gbm_surf)
			still_busy++;

	eglMakeCurrent(p->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	struct retiring_surface *parked = NULL;
	if (still_busy > 0)
		parked = calloc(1, sizeof(*parked));

	if (parked) {
		parked->gbm_surf = p->gbm_surf;
		parked->egl_surf = p->egl_surf;
		parked->outstanding = still_busy;
		parked->next = p->retiring;
		p->retiring = parked;
	} else {
		/* No buffers in flight for this surface, or calloc failed.
		 * Destroy THIS surface's buffers and the surface now, leaving
		 * any buffers owned by earlier parked surfaces alone. If
		 * still_busy > 0 here (the OOM case), the compositor's dmabuf
		 * import still holds an independent reference to the displayed
		 * pages, so this is safe. */
		for (int i = 0; i < NUM_BUFFERS; i++) {
			if (p->frame_bufs[i].owner != p->gbm_surf) continue;
			if (p->frame_bufs[i].wl_buf) {
				wl_buffer_destroy(p->frame_bufs[i].wl_buf);
				p->frame_bufs[i].wl_buf = NULL;
			}
			if (p->frame_bufs[i].bo) {
				gbm_surface_release_buffer(p->gbm_surf, p->frame_bufs[i].bo);
				p->frame_bufs[i].bo = NULL;
			}
			p->frame_bufs[i].busy = false;
			p->frame_bufs[i].owner = NULL;
		}
		if (p->egl_surf != EGL_NO_SURFACE)
			eglDestroySurface(p->egl_dpy, p->egl_surf);
		if (p->gbm_surf)
			gbm_surface_destroy(p->gbm_surf);
	}

	p->egl_surf = EGL_NO_SURFACE;
	p->gbm_surf = NULL;
	p->gbm_surf = gbm_surf_create_with_fallback(p->gbm_dev, nw, nh);

	if (!p->gbm_surf) {
		LOG("[wl-output] resize: gbm_surface_create %dx%d failed", nw, nh);
		return false;
	}
	p->egl_surf = eglCreateWindowSurface(p->egl_dpy, p->egl_cfg,
		(EGLNativeWindowType)p->gbm_surf, NULL);
	if (p->egl_surf == EGL_NO_SURFACE) {
		LOG("[wl-output] resize: eglCreateWindowSurface failed: 0x%x",
			eglGetError());
		return false;
	}

	p->output_width = nw;
	p->output_height = nh;

	/* Re-arm. Without this the output stays idle after a resize. */
	p->wl_due = 1;
	return true;
}

static bool wl_blit_and_commit(struct wl_priv *p, GLuint scene_tex,
                               bool schedule_callback)
{
	if (!eglMakeCurrent(p->egl_dpy, p->egl_surf, p->egl_surf, p->egl_ctx)) {
		LOG("[wl-output] make current failed: 0x%x", eglGetError());
		return false;
	}
	glViewport(0, 0, p->output_width, p->output_height);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	gl_quad_blit(scene_tex, 0, 0, p->output_width, p->output_height,
                        p->output_width, p->output_height, 1.0f);

	if (!eglSwapBuffers(p->egl_dpy, p->egl_surf)) {
		LOG("[wl-output] eglSwapBuffers failed: 0x%x", eglGetError());
		return false;
	}

	int slot = -1;
	for (int i = 0; i < NUM_BUFFERS; i++) {
		int idx = (p->current_buf + i) % NUM_BUFFERS;
		if (!p->frame_bufs[idx].busy) { slot = idx; break; }
	}
	if (slot < 0) { DBG("[wl-output] all buffers busy, skip"); return false; }

	if (p->frame_bufs[slot].bo) {
		gbm_surface_release_buffer(
			p->frame_bufs[slot].owner ? p->frame_bufs[slot].owner : p->gbm_surf,
			p->frame_bufs[slot].bo);
		p->frame_bufs[slot].bo = NULL;
	}

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(p->gbm_surf);
	if (!bo) { LOG("[wl-output] lock_front_buffer failed"); return false; }

	uint32_t bo_w = gbm_bo_get_width(bo), bo_h = gbm_bo_get_height(bo);
	uint32_t bo_f = gbm_bo_get_format(bo), bo_s = gbm_bo_get_stride(bo);
	uint64_t bo_m = gbm_bo_get_modifier(bo);
	int bo_fd = gbm_bo_get_fd(bo);
	if (bo_fd < 0) {
		LOG("[wl-output] gbm_bo_get_fd failed");
		gbm_surface_release_buffer(p->gbm_surf, bo);
		return false;
	}

	struct zwp_linux_buffer_params_v1 *params =
		zwp_linux_dmabuf_v1_create_params(p->dmabuf_iface);

	zwp_linux_buffer_params_v1_add(params, bo_fd, 0, 0, bo_s,
	                               bo_m >> 32, bo_m & 0xFFFFFFFF);

	/* The compositor samples XRGB8888 dmabuf with R/B transposed vs
	 * display controller, advertise XBGR8888. Do not revert to bo_f. */
	uint32_t wl_fourcc = (bo_f == GBM_FORMAT_XRGB8888) ? GBM_FORMAT_XBGR8888
				       : (bo_f == GBM_FORMAT_ARGB8888) ? GBM_FORMAT_ABGR8888
				       : bo_f;
	struct wl_buffer *wl_buf = zwp_linux_buffer_params_v1_create_immed(
		params, bo_w, bo_h, wl_fourcc, 0);

	zwp_linux_buffer_params_v1_destroy(params);
	close(bo_fd);

	if (!wl_buf) {
		LOG("[wl-output] create_immed NULL — compositor rejected dmabuf");
		gbm_surface_release_buffer(p->gbm_surf, bo);
		return false;
	}

	struct frame_buffer *fb = &p->frame_bufs[slot];
	fb->wl_buf = wl_buf;
	fb->bo = bo;
	fb->owner = p->gbm_surf;
	fb->priv = p;
	fb->busy = true;
	p->current_buf = (slot + 1) % NUM_BUFFERS;
	wl_buffer_add_listener(wl_buf, &buffer_listener, fb);

	wl_surface_attach(p->wl_surf, wl_buf, 0, 0);
	wl_surface_damage_buffer(p->wl_surf, 0, 0, INT32_MAX, INT32_MAX);

	/* Only the cadence path arms a frame callback. The synchronous resize
	 * path does not (it would double-arm and outrun the loop). */
	if (schedule_callback) {
		p->frame_cb = wl_surface_frame(p->wl_surf);
		wl_callback_add_listener(p->frame_cb, &frame_listener, p);
		p->frame_scheduled = true;
	}

	wl_surface_commit(p->wl_surf);
	wl_display_flush(p->wl_dpy);
	return true;
}

static bool output_wayland_present(struct output *c, const struct frame *f)
{
	struct wl_priv *p = c->priv;

	/* Cache the scene texture so the configure handler can re-blit it
	 * synchronously during a resize. */
	p->last_tex = f->gl_tex;
	p->last_master_w = f->width;
	p->last_master_h = f->height;

	return wl_blit_and_commit(p, f->gl_tex, true);
}

static int output_wayland_get_fd(struct output *c) {
	struct wl_priv *p = c->priv;
	return wl_display_get_fd(p->wl_dpy);
}
static void output_wayland_dispatch_events(struct output *c) {
	struct wl_priv *p = c->priv;

	while (wl_display_prepare_read(p->wl_dpy) != 0)
		wl_display_dispatch_pending(p->wl_dpy);

	wl_display_flush(p->wl_dpy);
	wl_display_read_events(p->wl_dpy);
	wl_display_dispatch_pending(p->wl_dpy);
}
static bool output_wayland_render_due(struct output *c) {
	return ((struct wl_priv *)c->priv)->wl_due;
}
static void output_wayland_mark_rendered(struct output *c) {
	((struct wl_priv *)c->priv)->wl_due = 0;
}
static int output_wayland_get_width(struct output *c) {
	return ((struct wl_priv *)c->priv)->output_width;
}
static int output_wayland_get_height(struct output *c) {
	return ((struct wl_priv *)c->priv)->output_height;
}
static int output_wayland_get_refresh_hz(struct output *c) {
	return ((struct wl_priv *)c->priv)->output_refresh_hz;
}

static void output_wayland_destroy(struct output *c) {
	if (!c) return;
	struct wl_priv *p = c->priv;
	if (p) {
		/* Drop the singleton FIRST so any concurrent accessor call from a
		 * teardown path elsewhere sees "no Wayland output" instead of a
		 * dangling priv. */
		if (g_wl_singleton == p) g_wl_singleton = NULL;

		if (p->frame_cb) wl_callback_destroy(p->frame_cb);
		for (int i = 0; i < NUM_BUFFERS; i++) {
			if (p->frame_bufs[i].wl_buf) wl_buffer_destroy(p->frame_bufs[i].wl_buf);
			if (p->frame_bufs[i].bo && p->frame_bufs[i].owner)
				gbm_surface_release_buffer(p->frame_bufs[i].owner, p->frame_bufs[i].bo);
		}
		/* EGL surface is ours. Context/display are the renderer's. */
		if (p->egl_dpy != EGL_NO_DISPLAY && p->egl_surf != EGL_NO_SURFACE) {
			eglMakeCurrent(p->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroySurface(p->egl_dpy, p->egl_surf);
		}
		if (p->gbm_surf) gbm_surface_destroy(p->gbm_surf);
		/* Retired surfaces that never finished draining. Teardown can't
		 * wait for releases. Compositor's dmabuf import keeps any still-
		 * displayed pages alive, so destroying them now is safe. */
		while (p->retiring) {
			struct retiring_surface *r = p->retiring;
			p->retiring = r->next;
			if (p->egl_dpy != EGL_NO_DISPLAY && r->egl_surf != EGL_NO_SURFACE)
				eglDestroySurface(p->egl_dpy, r->egl_surf);
			if (r->gbm_surf) gbm_surface_destroy(r->gbm_surf);
			free(r);
		}
		/* gbm_dev is the renderer's. Do NOT destroy. */
		if (p->layer_surface) zwlr_layer_surface_v1_destroy(p->layer_surface);
		if (p->xdg_top) xdg_toplevel_destroy(p->xdg_top);
		if (p->xdg_surf) xdg_surface_destroy(p->xdg_surf);
		if (p->wl_surf) wl_surface_destroy(p->wl_surf);
		if (p->wl_out) wl_output_destroy(p->wl_out);
		if (p->wl_shm) wl_shm_destroy(p->wl_shm);
		if (p->wl_comp) wl_compositor_destroy(p->wl_comp);
		if (p->wl_reg) wl_registry_destroy(p->wl_reg);
		if (p->wl_dpy) wl_display_disconnect(p->wl_dpy);
		free(p);
	}
	free(c);
}

struct output *output_wayland_create(output_role_t role) {
	struct output *c = calloc(1, sizeof(*c));
	if (!c) return NULL;
	struct wl_priv *p = calloc(1, sizeof(*p));
	if (!p) { free(c); return NULL; }

	/* Map the generic surface role to the Wayland shell role. AUTO defers
	 * the wallpaper/windowed choice to init. */

	switch (role) {
	case OUTPUT_ROLE_WALLPAPER: p->sub_mode = WL_ROLE_WALLPAPER; break;
	case OUTPUT_ROLE_WINDOWED: p->sub_mode = WL_ROLE_WINDOWED; break;
	case OUTPUT_ROLE_AUTO:
	default: p->sub_mode = WL_ROLE_AUTO; break;
	}

	p->egl_dpy = EGL_NO_DISPLAY;
	p->egl_surf = EGL_NO_SURFACE;

	c->type = OUTPUT_WAYLAND;
	c->needs_recomposite = true;
	c->priv = p;
	c->init = output_wayland_init;
	c->destroy = output_wayland_destroy;
	c->present = output_wayland_present;
	c->get_fd = output_wayland_get_fd;
	c->dispatch_events = output_wayland_dispatch_events;
	c->render_due = output_wayland_render_due;
	c->mark_rendered = output_wayland_mark_rendered;
	c->get_width = output_wayland_get_width;
	c->get_height = output_wayland_get_height;
	c->get_refresh_hz = output_wayland_get_refresh_hz;
	return c;
}

struct wl_compositor *wayland_get_compositor(void) {
	return g_wl_singleton ? g_wl_singleton->wl_comp : NULL;
}
struct wl_shm *wayland_get_shm(void) {
	return g_wl_singleton ? g_wl_singleton->wl_shm : NULL;
}
struct wl_output *wayland_get_output(void) {
	return g_wl_singleton ? g_wl_singleton->wl_out : NULL;
}
struct zwlr_layer_shell_v1 *wayland_get_layer_shell(void) {
	return g_wl_singleton ? g_wl_singleton->layer_shell : NULL;
}
