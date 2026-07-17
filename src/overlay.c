// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file overlay.c
 * @brief Overlay rendering.
 *
 * Renders now-playing text and album art with cairo into a layer the
 * scene composites over the frame. */

#define _GNU_SOURCE
#include "overlay.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "gl_quad.h"
#include "color.h"
#include "module_registry.h"
#include "app_paths.h"
#include "art_decode.h"
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <cairo/cairo.h>

static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct zwlr_layer_shell_v1 *g_layer_shell;
static struct wl_output *g_output;

static int g_out_w, g_out_h;
static int g_surf_w, g_surf_h;
static int g_inited;

static struct overlay_style g_style;

static struct wl_surface *g_surface;
static struct zwlr_layer_surface_v1 *g_layer_surface;
static unsigned char *g_pixels;
static int g_configured;
static int g_live; // surface is mapped

/* When `g_style.layer` is SCENE_LAYER_BACK we don't create a Wayland surface.
 * Instead we render Cairo into a malloc'd RGBA buffer, upload it as a GL
 * texture on change, and draw it via `overlay_render_present` each frame. */
static int g_gl_mode; // 1 if in GL/in-scene mode
static GLuint g_gl_tex; // texture for the Cairo composite
static int g_tex_dirty;
static int g_screen_x; // top-left position on output, pixels
static int g_screen_y;

/* Finished string handed over by the caller. */
#define VIEW_TEXT_MAX 4096

static char g_view_text[VIEW_TEXT_MAX];
static int g_view_show_art; // Render cached art with the current view

/* Art cache. Decoded once on main thread, cached. We cache by path and
 * mtime because remote art is written to the same /tmp filename for every
 * track. The path is not unique. Cache lifetime is
 * independent of view visibility, so peek can still composite it after
 * the HUD's now-playing line clears. */
static cairo_surface_t *g_art_surface;
static char g_art_loaded_path[1024];
static time_t g_art_loaded_mtime;

/* Mutex-protected request queue between any-thread setters and main-thread tick */
static pthread_mutex_t g_req_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_req_view_dirty;
static char g_req_view_text[VIEW_TEXT_MAX];
static int g_req_view_show_art;

static int g_req_art_dirty; // new art path pending
static char g_req_art_path[1024]; // "" = clear

static int g_req_transient_pending;
static char g_req_transient_text[2048];
static int g_req_transient_duration;

static int g_req_overlay_pending;
static struct overlay_spec g_req_overlay;

static int g_req_style_pending;
static struct overlay_style g_req_style;

/* Active transient. What's on screen right now if non-empty. */
static char g_active_transient[2048];
static int g_active_transient_duration;
static struct timespec g_active_transient_start;
static int g_active_show_art;

/* Active overlay state */
static cairo_surface_t *g_active_overlay_art;
static int g_active_art_placement; // -1 = style default
static float g_active_art_alpha; // -1.0 = style default
static float g_active_art_size_frac; // -1.0 = style default
static int g_active_burn; // -1 = style default; 0/1 = override

/* Active geometry overrides */
static coord_length_t g_active_pos_x;
static coord_length_t g_active_pos_y;
static coord_length_t g_active_size_w;
static coord_length_t g_active_size_h;
static int g_active_anchor = OVERLAY_SPEC_DEFAULT;

/* Active layer override */
static int g_active_layer = OVERLAY_SPEC_DEFAULT;

/* Resolved text style for the transient on screen. The composer reads these,
 * never `g_style` directly, so an overlay's text overrides layer cleanly. */
struct text_style {
	char font_family[128];
	double font_size;
	float color_r, color_g, color_b, color_a;
	float stroke_r, stroke_g, stroke_b, stroke_a;
	double stroke_width;
};
static struct text_style g_rtext;

/* Fade phases. Smoothstep alpha curve. The burn snapshot fires at FADE_OUT
 * entry and then runs on its own wall-clock timeline, outliving the fade. */
enum overlay_phase {
	OVERLAY_PHASE_NONE = 0,
	OVERLAY_PHASE_FADE_IN,
	OVERLAY_PHASE_STEADY,
	OVERLAY_PHASE_FADE_OUT
};

static int g_fade_in_ms_target; // config knob (cached)
static int g_fade_out_ms_target;

/* Per-transient effective fade durations (snapshot at start so config reload
 * mid-fade doesn't yank the curve). */
static int g_fade_in_ms_active;
static int g_fade_out_ms_active;

static int g_visible_phase; // enum overlay_phase. int for clean static-zero init
static struct timespec g_phase_start; // monotonic. reset on phase entry
static float g_visible_alpha = 1.0f;

static int g_view_was_visible; // tracks prev tick for transitions

/* Burn-in. On info-peek expiry, snapshot overlay pixels into a GL
 * texture, then sine-bell-fade blend it into the visualizer's feedback
 * canvas each frame. Wall-clock based. Rate-independent. Per-frame work is
 * GPU-side via the burn shader (BGRA->RGBA swizzle + Y-flip + alpha multiply
 * in one pass). */
static int g_burn_active;
static struct timespec g_burn_start_time; // wall-clock snapshot instant
static int g_burn_ms_target = 1500; // wall-clock duration knob, set via overlay_set_burn_ms
static GLuint g_burn_tex;
static int g_burn_tex_w, g_burn_tex_h;
static int g_burn_rect_x, g_burn_rect_y, g_burn_rect_w, g_burn_rect_h;

/* Scratch FBO for the per-frame burn alpha-multiply pass. */
static GLuint g_burn_scratch_fbo;
static GLuint g_burn_scratch_tex;
static int g_burn_scratch_w, g_burn_scratch_h;

/* Burn shader: BGRA->RGBA swizzle + Y-flip + alpha multiply in one fragment.
 * Lazy-compiled on first use. */
static GLuint g_burn_prog;
static GLint g_burn_prog_alpha_loc = -1;
static GLint g_burn_prog_tex_loc = -1;
static GLuint g_burn_vao;

static int g_dirty;

/* Layer surface listener */
static void on_configure(void *data,
	struct zwlr_layer_surface_v1 *surface,
	uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data; (void)w; (void)h;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	g_configured = 1;
}

static void on_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	(void)data; (void)surface;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
	.configure = on_configure,
	.closed = on_closed,
};

static struct wl_buffer
*create_shm_buffer(int w, int h, unsigned char **out_data, int *out_size) {
	int stride = w * 4;
	int size = stride * h;

	int fd = memfd_create("overlay", 0);
	if (fd < 0) return NULL;
	if (ftruncate(fd, size) < 0) { close(fd); return NULL; }

	unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) { close(fd); return NULL; }

	struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                          WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	*out_data = data;
	*out_size = size;
	return buf;
}

/* SHM buffer pool (FRONT mode double-buffering).
 * Wayland requires that an attached+committed buffer isn't modified until
 * wl_buffer.release. Two-buffer ping-pong prevents torn fades. */
#define OVERLAY_SHM_BUFS 2
struct shm_pool_buf {
	struct wl_buffer *wl_buf;
	unsigned char *pixels;
	int size;
	int busy; // 1 while compositor holds it
};
static struct shm_pool_buf g_shm_pool[OVERLAY_SHM_BUFS];
static int g_shm_active = -1; // index into g_shm_pool, -1 = none

/* Frame-callback gate (FRONT mode). Don't commit faster than the
 * compositor can present. Register wl_surface_frame after each commit. */
static struct wl_callback *g_overlay_frame_cb = NULL;
static int g_overlay_frame_pending = 0;

static void shm_buf_release(void *data, struct wl_buffer *wl_buf) {
	(void)wl_buf;
	struct shm_pool_buf *b = data;
	if (b) b->busy = 0;
}
static const struct wl_buffer_listener g_shm_buf_listener = {
	.release = shm_buf_release,
};

/* Overlay surface frame callback (FRONT mode). */
static void overlay_frame_done(void *data, struct wl_callback *cb, uint32_t t) {
	(void)data; (void)t;
	if (cb) wl_callback_destroy(cb);
	g_overlay_frame_cb = NULL;
	g_overlay_frame_pending = 0;
}
static const struct wl_callback_listener g_overlay_frame_listener = {
	.done = overlay_frame_done,
};

static void destroy_surface(void) {
	if (g_gl_mode) {
		if (g_gl_tex) { glDeleteTextures(1, &g_gl_tex); g_gl_tex = 0; }
		if (g_pixels) { free(g_pixels); g_pixels = NULL; }
		g_gl_mode = 0;
		g_tex_dirty = 0;
	} else {
		/* Tear down the SHM ping-pong pool. `g_pixels` was an alias into one
		 * of these mmaps. Clear it to avoid a dangling pointer. */
		if (g_overlay_frame_cb) {
			wl_callback_destroy(g_overlay_frame_cb);
			g_overlay_frame_cb = NULL;
	        }
		g_overlay_frame_pending = 0;
		for (int i = 0; i < OVERLAY_SHM_BUFS; i++) {
			    struct shm_pool_buf *b = &g_shm_pool[i];
			    if (b->wl_buf) { wl_buffer_destroy(b->wl_buf); b->wl_buf = NULL; }
			    if (b->pixels) { munmap(b->pixels, b->size); b->pixels = NULL; }
			    b->size = 0;
			    b->busy = 0;
	        }
		g_shm_active = -1;
		g_pixels = NULL;
		if (g_layer_surface) {
			zwlr_layer_surface_v1_destroy(g_layer_surface);
			g_layer_surface = NULL;
		}
			if (g_surface) { wl_surface_destroy(g_surface); g_surface = NULL; }
	}
	g_live = 0;
	g_configured = 0;
}

static void resolve_overlay_rect(coord_rect_t *out) {
	coord_length_t px = g_active_pos_x.present ? g_active_pos_x : g_style.pos_x;
	coord_length_t py = g_active_pos_y.present ? g_active_pos_y : g_style.pos_y;
	coord_length_t sw = g_active_size_w.present ? g_active_size_w : g_style.size_w;
	coord_length_t sh = g_active_size_h.present ? g_active_size_h : g_style.size_h;
	coord_anchor_t an = (g_active_anchor != OVERLAY_SPEC_DEFAULT)
	                     ? (coord_anchor_t)g_active_anchor
	                     : g_style.anchor;
	coord_resolve_rect(px, py, sw, sh, an, COORD_ASPECT_STRETCH,
	                   g_out_w, g_out_h, 0, 0, out);
}

static void compute_geometry(void) {
	coord_rect_t r;
	resolve_overlay_rect(&r);
	g_surf_w = r.w;
	g_surf_h = r.h;
	if (g_surf_w < 32) g_surf_w = 32;
	if (g_surf_h < 32) g_surf_h = 32;
}

static void compute_position(void) {
	coord_rect_t r;
	resolve_overlay_rect(&r);
	int x = r.x;
	int y = r.y;
	/* Clamp so the surface isn't entirely off-screen. */
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > g_out_w - 32) x = g_out_w - 32;
	if (y > g_out_h - 32) y = g_out_h - 32;
	g_screen_x = x;
	g_screen_y = y;
}

/* Layer-shell namespace for the overlay surface. Derived from `APP_ID` via
 * app_paths. */
static const char *overlay_layer_namespace(void) {
	static char ns[256];
	if (!ns[0])
		snprintf(ns, sizeof(ns), "%.*s-overlay",
		         (int)(sizeof(ns) - 10), app_paths_app_name());
	return ns;
}
static int create_surface(void) {
	compute_geometry();
	compute_position();

	/* Effective layer: overlay override wins, else style. The override is
	 * sentinel-cleared when the transient ends, so later surface creations
	 * fall back to style on their own. */
	int eff_layer = (g_active_layer != OVERLAY_SPEC_DEFAULT)
				  ? g_active_layer : g_style.layer;

	if (eff_layer == SCENE_LAYER_FRONT && !g_layer_shell) {
		fprintf(stderr, "[overlay] FRONT requested but no layer_shell "
		        "available; downgrading to BACK\n");
		eff_layer = SCENE_LAYER_BACK;
	}

	/* No Wayland surface. overlay_render_present draws our texture via
	 * gl_quad_blit each frame. */
	if (eff_layer == SCENE_LAYER_BACK) {
		int stride = g_surf_w * 4;
		int size = stride * g_surf_h;
		g_pixels = calloc(1, size);
		if (!g_pixels) return 0;

		glGenTextures(1, &g_gl_tex);
		if (!g_gl_tex) {
			free(g_pixels);
			g_pixels = NULL;
			return 0;
		}
		glBindTexture(GL_TEXTURE_2D, g_gl_tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		/* Allocate storage. Data uploaded on first dirty. */
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_surf_w, g_surf_h, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);

		g_gl_mode = 1;
		g_tex_dirty = 1; // first render will populate
		g_configured = 1; // GL mode is configured immediately
		g_live = 1;
		return 1;
	}

	/* Above-scene mode */
	g_surface = wl_compositor_create_surface(g_comp);
	if (!g_surface) return 0;

	g_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		g_layer_shell, g_surface, g_output,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, overlay_layer_namespace());

	zwlr_layer_surface_v1_set_anchor(g_layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_size(g_layer_surface, g_surf_w, g_surf_h);
	zwlr_layer_surface_v1_set_margin(g_layer_surface,
		g_screen_y, 0, 0, g_screen_x);
	zwlr_layer_surface_v1_set_exclusive_zone(g_layer_surface, -1);

	zwlr_layer_surface_v1_add_listener(g_layer_surface, &ls_listener, NULL);

	/* Empty input region. Clicks pass through */
	struct wl_region *region = wl_compositor_create_region(g_comp);
	wl_surface_set_input_region(g_surface, region);
	wl_region_destroy(region);

	/* Allocate the double-buffer pool. Two buffers of the same size, each
	 * with the wl_buffer.release listener so commit_buffer can rotate to a
	 * free one. g_pixels initially aliases slot 0's mmap. commit_buffer
	 * reseats it on every rotation. */
	for (int i = 0; i < OVERLAY_SHM_BUFS; i++) {
		struct shm_pool_buf *b = &g_shm_pool[i];
		b->wl_buf = create_shm_buffer(g_surf_w, g_surf_h, &b->pixels, &b->size);
		if (!b->wl_buf) {
			destroy_surface();
			return 0;
		}
		b->busy = 0;
		wl_buffer_add_listener(b->wl_buf, &g_shm_buf_listener, b);
	}
	g_shm_active = 0;
	g_pixels = g_shm_pool[0].pixels;

	g_configured = 0;
	g_live = 1;
	wl_surface_commit(g_surface);
	return 1;
}

/* Fill `g_rtext` from the base style alone */
static void resolve_text_style_base(void) {
	snprintf(g_rtext.font_family, sizeof(g_rtext.font_family),
	         "%s", g_style.font_family);
	g_rtext.font_size = g_style.font_size;
	g_rtext.color_r = g_style.color_r;
	g_rtext.color_g = g_style.color_g;
	g_rtext.color_b = g_style.color_b;
	g_rtext.color_a = g_style.color_a;
	g_rtext.stroke_r = g_style.stroke_r;
	g_rtext.stroke_g = g_style.stroke_g;
	g_rtext.stroke_b = g_style.stroke_b;
	g_rtext.stroke_a = g_style.stroke_a;
	g_rtext.stroke_width = g_style.stroke_width;
}

/* Fill `g_rtext` from base style patched by an overlay's text overrides */
static void resolve_text_style_overlay(const struct overlay_spec *d) {
	resolve_text_style_base();
	if (d->font[0])
		snprintf(g_rtext.font_family, sizeof(g_rtext.font_family), "%s", d->font);
	if (d->text_size > 0)
		g_rtext.font_size = d->text_size;
	if (d->stroke_width >= 0)
		g_rtext.stroke_width = d->stroke_width;
	if (d->text_color[0])
		color_parse_hex(d->text_color, &g_rtext.color_r, &g_rtext.color_g,
				        &g_rtext.color_b, &g_rtext.color_a);
	if (d->stroke_color[0])
		color_parse_hex(d->stroke_color, &g_rtext.stroke_r, &g_rtext.stroke_g,
				        &g_rtext.stroke_b, &g_rtext.stroke_a);
}

/* Draw an array of text lines, top-down, centered horizontally, starting at y. */
static double draw_text_lines(cairo_t *cr, char **lines, int nlines,
                              double cx, double y_top, double line_height,
                              cairo_font_extents_t *fe)
{
	double y = y_top + fe->ascent;
	for (int i = 0; i < nlines; i++) {
		cairo_text_extents_t te;
		cairo_text_extents(cr, lines[i], &te);
		double x = cx - te.width / 2.0 - te.x_bearing;

		if (g_rtext.stroke_width > 0 && g_rtext.stroke_a > 0) {
			cairo_move_to(cr, x, y);
			cairo_text_path(cr, lines[i]);
			cairo_set_source_rgba(cr,
				g_rtext.stroke_r, g_rtext.stroke_g,
				g_rtext.stroke_b, g_rtext.stroke_a);
			cairo_set_line_width(cr, g_rtext.stroke_width);
			cairo_stroke(cr);
		}
		cairo_set_source_rgba(cr,
			g_rtext.color_r, g_rtext.color_g,
			g_rtext.color_b, g_rtext.color_a);
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, lines[i]);

		y += line_height;
	}
	return y;
}

/* Split a string on '\n' into a lines array (modifies the string in place). */
static int split_lines(char *s, char **lines, int max) {
	int n = 0;
	char *saveptr = NULL;
	char *tok = strtok_r(s, "\n", &saveptr);
	while (tok && n < max) {
		lines[n++] = tok;
		tok = strtok_r(NULL, "\n", &saveptr);
	}
	return n;
}

/* Paint art into a target rect (x, y, w, h), aspect-preserving contain, at
 * the given alpha. alpha=1.0 is opaque, 0.0 is invisible. */
static void paint_art(cairo_t *cr, cairo_surface_t *art,
				      double x, double y, double w, double h, float alpha)
{
	if (!art) return;
	if (alpha <= 0.0f) return;
	if (alpha > 1.0f) alpha = 1.0f;

	int iw = cairo_image_surface_get_width(art);
	int ih = cairo_image_surface_get_height(art);
	if (iw <= 0 || ih <= 0) return;

	double sx = w / iw;
	double sy = h / ih;
	double s = sx < sy ? sx : sy;
	double dw = iw * s;
	double dh = ih * s;
	double dx = x + (w - dw) / 2.0;
	double dy = y + (h - dh) / 2.0;

	cairo_save(cr);
	cairo_translate(cr, dx, dy);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, art, 0, 0);
	cairo_paint_with_alpha(cr, alpha);
	cairo_restore(cr);
}

/* Render text + optional art into `g_pixels`.
 * Art occupies a square of `art_size_frac` * min(surf_w, surf_h), fit
 * aspect-preserving. Placement determines art position relative to text. */
static void render_composed(char **lines, int nlines,
				            cairo_surface_t *art, int show_art)
{
	if (!g_pixels) return;
	int stride = g_surf_w * 4;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		g_pixels, CAIRO_FORMAT_ARGB32, g_surf_w, g_surf_h, stride);
	cairo_t *cr = cairo_create(cs);

	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_select_font_face(cr, g_rtext.font_family,
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, g_rtext.font_size);

	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	double line_height = fe.height * 1.3;
	double text_block_h = nlines * line_height;

	int placement = show_art && art ? g_style.art_placement : -1;
	/* An overlay's own override wins when set. Sentinel -1 means "use style". */
	if (placement >= 0 && g_active_art_placement >= 0)
		placement = g_active_art_placement;

	/* Square bounding box, side = art_size_frac * min(surface dims).
	 * Same rule for every placement - including BEHIND, so a wide overlay
	 * with a square cover doesn't have to span the full surface width. */
	double art_side = 0;
	if (placement >= 0) {
		double frac = (g_active_art_size_frac > 0.0f)
			? (double)g_active_art_size_frac
			: (g_style.art_size_frac > 0 ? g_style.art_size_frac : 0.75);
		if (frac > 1.0) frac = 1.0;
		double min_dim = g_surf_w < g_surf_h ? g_surf_w : g_surf_h;
		art_side = min_dim * frac;
	}
	double art_alpha_eff = (g_active_art_alpha >= 0.0f)
		? (double)g_active_art_alpha
		: g_style.art_alpha;

	const double GAP = 8.0; // space between art and text for non-BEHIND

	double cx = g_surf_w / 2.0;
	double text_y_top;

	switch (placement) {
	case OVERLAY_ART_BEHIND: {
		double ax = (g_surf_w - art_side) / 2.0;
		double ay = (g_surf_h - art_side) / 2.0;
		paint_art(cr, art, ax, ay, art_side, art_side, art_alpha_eff);
		text_y_top = (g_surf_h - text_block_h) / 2.0;
		break;
	}
	case OVERLAY_ART_ABOVE: {
		double total_h = art_side + GAP + text_block_h;
		double top = (g_surf_h - total_h) / 2.0;
		if (top < 0) top = 0;
		paint_art(cr, art, (g_surf_w - art_side) / 2.0, top,
				  art_side, art_side, art_alpha_eff);
		text_y_top = top + art_side + GAP;
		break;
	}
	case OVERLAY_ART_BELOW: {
		double total_h = text_block_h + GAP + art_side;
		text_y_top = (g_surf_h - total_h) / 2.0;
		if (text_y_top < 0) text_y_top = 0;
		paint_art(cr, art, (g_surf_w - art_side) / 2.0,
				  text_y_top + text_block_h + GAP,
				  art_side, art_side, art_alpha_eff);
		break;
	}
	case OVERLAY_ART_LEFT: {
		double ax = GAP;
		double ay = (g_surf_h - art_side) / 2.0;
		paint_art(cr, art, ax, ay, art_side, art_side, art_alpha_eff);
		/* Text region spans [ax + art_side + GAP, surf_w] - center it there */
		cx = (ax + art_side + GAP + g_surf_w) / 2.0;
		text_y_top = (g_surf_h - text_block_h) / 2.0;
		break;
	}
	case OVERLAY_ART_RIGHT: {
		double ay = (g_surf_h - art_side) / 2.0;
		double ax = g_surf_w - art_side - GAP;
		if (ax < 0) ax = 0;
		paint_art(cr, art, ax, ay, art_side, art_side, art_alpha_eff);
		cx = ax / 2.0;
		text_y_top = (g_surf_h - text_block_h) / 2.0;
		break;
	}
	default:
		text_y_top = (g_surf_h - text_block_h) / 2.0;
		break;
	}

	draw_text_lines(cr, lines, nlines, cx, text_y_top, line_height, &fe);

	cairo_surface_flush(cs);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	/* Wayland-mode (above-scene) fade: GL mode applies u_alpha at draw time
	 * via a shader uniform. The Wayland compositor has no such hook, so we
	 * scale the pixel buffer in-place after Cairo paints. Source is
	 * premultiplied ARGB32 - multiplying all four channels by the same
	 * scalar preserves the premultiplied invariant. No-op when alpha is 1
	 * (steady state) since the loop short-circuits. */
	if (!g_gl_mode && g_visible_alpha < 0.999f && g_pixels) {
		float a = g_visible_alpha < 0.0f ? 0.0f : g_visible_alpha;
		unsigned int ai = (unsigned int)(a * 256.0f); // 0..256
		if (ai > 256) ai = 256;
		size_t npix = (size_t)g_surf_w * (size_t)g_surf_h;
		uint8_t *p = g_pixels;
		for (size_t i = 0; i < npix; i++) {
			p[0] = (uint8_t)((p[0] * ai) >> 8);
			p[1] = (uint8_t)((p[1] * ai) >> 8);
			p[2] = (uint8_t)((p[2] * ai) >> 8);
			p[3] = (uint8_t)((p[3] * ai) >> 8);
			p += 4;
		}
	}
}

/* Render the view. The string and show-art decision arrive finished
 * from the caller, overlay just splits to lines and paints. Art shows
 * when this view asked for it and a bitmap is cached. */
static void render_view(void) {
	resolve_text_style_base();

	if (!g_view_text[0]) return;

	char buf[VIEW_TEXT_MAX];
	snprintf(buf, sizeof(buf), "%s", g_view_text);

	char *lines[64];
	int nlines = split_lines(buf, lines, 64);

	cairo_surface_t *art = (g_view_show_art && g_art_surface)
				           ? g_art_surface : NULL;

	render_composed(lines, nlines, art, art != NULL);
}

static void render_transient(const char *text, int show_art) {
	char copy[2048];
	snprintf(copy, sizeof(copy), "%s", text);
	char *lines[32];
	int nlines = split_lines(copy, lines, 32);
	/* Art preference: overlay's own image > cached now-playing > none. */
	cairo_surface_t *art;
	if (g_active_overlay_art) {
		art = g_active_overlay_art;
	} else if (show_art && g_art_surface) {
		art = g_art_surface;
	} else {
		art = NULL;
	}
	render_composed(lines, nlines, art, art != NULL);
}

/* Fade helpers.
 * State machine: NONE -> FADE_IN -> STEADY -> FADE_OUT -> NONE.
 * Burn snapshot at FADE_OUT entry (decoupled from surface destroy). */

/* Smoothstep curve on linear progress [0,1] -> eased [0,1]. */
static float smoothstep01(float s) {
	if (s <= 0.0f) return 0.0f;
	if (s >= 1.0f) return 1.0f;
	return s * s * (3.0f - 2.0f * s);
}

static double phase_elapsed_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - g_phase_start.tv_sec) * 1000.0
		 + (now.tv_nsec - g_phase_start.tv_nsec) / 1e6;
}

/* Resolve effective per-phase fade durations, scaling down proportionally
 * if fade_in + fade_out >= total budget. */
static void resolve_fade_durations(int *fi_out, int *fo_out) {
	int fi = g_fade_in_ms_active > 0 ? g_fade_in_ms_active : 0;
	int fo = g_fade_out_ms_active > 0 ? g_fade_out_ms_active : 0;
	int total = g_active_transient_duration > 0 ? g_active_transient_duration : 0;
	if (fi + fo >= total && total > 0 && (fi + fo) > 0) {
		/* Scale both fades to share the budget. Min 1 ms each to avoid /0. */
		double scale = (double)total / (double)(fi + fo);
		fi = (int)(fi * scale);
		fo = (int)(fo * scale);
		if (fi < 0) fi = 0;
		if (fo < 0) fo = 0;
		/* Steady-less but valid bookends: steady starts at fi and ends at
		 * total-fo, so if fi + fo equals total exactly, steady has zero duration
		 * yet the boundary checks still hold. */
	}
	*fi_out = fi;
	*fo_out = fo;
}

/* Called every tick. Advances the fade state machine, sets `g_visible_alpha`,
 * and returns 1 if entering FADE_OUT this tick (caller decides whether to
 * arm the burn - only when the stack is draining to empty). */
static int fade_tick_advance(void) {
	if (!g_active_transient[0]) {
		/* No transient active - `view_tick_advance` owns phase and alpha. */
		return 0;
	}

	int fi, fo;
	resolve_fade_durations(&fi, &fo);
	int total = g_active_transient_duration;

	/* Boundary milestones, from the original transient_start: */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double from_start_ms =
		(now.tv_sec - g_active_transient_start.tv_sec) * 1000.0 +
		(now.tv_nsec - g_active_transient_start.tv_nsec) / 1e6;

	int entered_fade_out = 0;

	/* Decide phase from elapsed-since-start. We may need to walk forward
	 * (FADE_IN -> STEADY -> FADE_OUT) in a single tick if a frame stalled. */
	enum overlay_phase new_phase;
	if (from_start_ms < fi) {
		new_phase = OVERLAY_PHASE_FADE_IN;
	} else if (from_start_ms < total - fo) {
		new_phase = OVERLAY_PHASE_STEADY;
	} else {
		new_phase = OVERLAY_PHASE_FADE_OUT;
	}

	/* Phase entry side effects: record g_phase_start, signal burn at FADE_OUT
	 * entry. We compare against g_visible_phase to detect transitions. */
	if ((int)new_phase != g_visible_phase) {
		/* For each newly-entered phase, set g_phase_start to the moment that
		 * phase actually began (in terms of from_start_ms), not "now" - so
		 * the smoothstep math uses the right offset even if we walked
		 * multiple phases in one tick. */
		struct timespec base = g_active_transient_start;

		#define ADD_MS_TO(out, ms_offset) do { \
			long ns = (long)((ms_offset) * 1e6); \
			(out).tv_sec = base.tv_sec + (ns / 1000000000L); \
			(out).tv_nsec = base.tv_nsec + (ns % 1000000000L); \
			if ((out).tv_nsec >= 1000000000L) { \
				(out).tv_sec += 1; \
				(out).tv_nsec -= 1000000000L; \
			} \
		} while (0)

		switch (new_phase) {
		case OVERLAY_PHASE_FADE_IN:
			g_phase_start = base;
			break;
		case OVERLAY_PHASE_STEADY:
			ADD_MS_TO(g_phase_start, fi);
			break;
		case OVERLAY_PHASE_FADE_OUT:
			ADD_MS_TO(g_phase_start, total - fo);
			if (g_visible_phase != (int)OVERLAY_PHASE_FADE_OUT) {
				entered_fade_out = 1;
			}
			break;
		default:
			g_phase_start = base;
			break;
		}

		#undef ADD_MS_TO

		g_visible_phase = new_phase;
	}

	/* Compute alpha in the current phase. */
	double ph_ms = phase_elapsed_ms();
	if (ph_ms < 0) ph_ms = 0;

	switch (g_visible_phase) {
	case OVERLAY_PHASE_FADE_IN: {
		float s = (fi > 0) ? (float)(ph_ms / (double)fi) : 1.0f;
		g_visible_alpha = smoothstep01(s);
		break;
	}
	case OVERLAY_PHASE_STEADY:
		g_visible_alpha = 1.0f;
		break;
	case OVERLAY_PHASE_FADE_OUT: {
		float s = (fo > 0) ? (float)(ph_ms / (double)fo) : 1.0f;
		g_visible_alpha = smoothstep01(1.0f - s);
		break;
	}
	default:
		g_visible_alpha = 0.0f;
		break;
	}

	return entered_fade_out;
}

/* View fade: FADE_IN on appearance, STEADY indefinitely, FADE_OUT when
 * the view clears. Returns 1 on FADE_OUT entry. */
static int view_tick_advance(int view_now_visible) {
	int entered_fade_out = 0;

	int fi = g_fade_in_ms_active;
	int fo = g_fade_out_ms_active;

	/* Detect transitions vs last tick */
	if (view_now_visible && !g_view_was_visible) {
		/* View just became visible from nothing - start FADE_IN.
		 * If we were mid-FADE_OUT (race: view reappeared during fade-out),
		 * pick up from the current alpha to avoid a jump. */
		if (g_visible_phase == (int)OVERLAY_PHASE_FADE_OUT && fi > 0) {
			/* Compute the phase_start that would yield the current alpha
			 * under the FADE_IN curve. Since smoothstep isn't trivially
			 * invertible, approximate via the linear progress. */
			float cur = g_visible_alpha < 0 ? 0 : (g_visible_alpha > 1 ? 1 : g_visible_alpha);
			/* Find s such that smoothstep01(s) ~= cur. Use a couple of
			 * Newton iterations on f(s)=s^2(3-2s)-cur, f'(s)=6s(1-s). */
			float s = cur;
			for (int i = 0; i < 4; i++) {
				float fs = s*s*(3.0f - 2.0f*s) - cur;
				float dfs = 6.0f*s*(1.0f - s);
				if (dfs > 1e-4f) s -= fs / dfs;
				if (s < 0) s = 0;
				if (s > 1) s = 1;
			}
			/* Now g_phase_start should be (now - s * fi) ms */
			double back_ms = (double)s * (double)fi;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long ns = (long)(back_ms * 1e6);
			long total_ns = (long)now.tv_nsec - (ns % 1000000000L);
			time_t sec_adj = now.tv_sec - (ns / 1000000000L);
			if (total_ns < 0) { total_ns += 1000000000L; sec_adj -= 1; }
			g_phase_start.tv_sec = sec_adj;
			g_phase_start.tv_nsec = total_ns;
		} else {
			clock_gettime(CLOCK_MONOTONIC, &g_phase_start);
		}
		g_visible_phase = OVERLAY_PHASE_FADE_IN;
	} else if (!view_now_visible && g_view_was_visible) {
		/* View just became invisible - start FADE_OUT. */
		clock_gettime(CLOCK_MONOTONIC, &g_phase_start);
		g_visible_phase = OVERLAY_PHASE_FADE_OUT;
		entered_fade_out = 1;
	}

	double ph_ms = phase_elapsed_ms();
	if (ph_ms < 0) ph_ms = 0;

	switch (g_visible_phase) {
	case OVERLAY_PHASE_FADE_IN: {
		float s = (fi > 0) ? (float)(ph_ms / (double)fi) : 1.0f;
		g_visible_alpha = smoothstep01(s);
		if (s >= 1.0f) g_visible_phase = OVERLAY_PHASE_STEADY;
		break;
	}
	case OVERLAY_PHASE_STEADY:
		g_visible_alpha = 1.0f;
		break;
	case OVERLAY_PHASE_FADE_OUT: {
		float s = (fo > 0) ? (float)(ph_ms / (double)fo) : 1.0f;
		g_visible_alpha = smoothstep01(1.0f - s);
		if (s >= 1.0f) g_visible_phase = OVERLAY_PHASE_NONE;
		break;
	}
	default:
		g_visible_alpha = view_now_visible ? 1.0f : 0.0f;
		break;
	}

	g_view_was_visible = view_now_visible;
	return entered_fade_out;
}

/* Rotate to a free SHM buffer before painting. Falls back to current
 * if both are busy (same hazard as single-buffer baseline). */
static int prepare_paint_target(void) {
	if (g_gl_mode) return 1; // GL mode: caller writes via g_pixels CPU buffer, no rotation
	if (g_shm_active < 0) return 0;
	for (int i = 0; i < OVERLAY_SHM_BUFS; i++) {
		int idx = (g_shm_active + 1 + i) % OVERLAY_SHM_BUFS;
		if (!g_shm_pool[idx].busy) {
			g_shm_active = idx;
			g_pixels = g_shm_pool[idx].pixels;
			return 1;
		}
	}
	/* All buffers busy - keep current slot. Compositor back-pressured. The
	 * paint will overwrite content the compositor may still be sampling
	 * (same hazard as the pre-pool single-buffer baseline). Rare in
	 * practice. Double-buffer + frame-callback pacing prevents it. */
	return 0;
}

/* Attach+commit the active SHM buffer and register a frame callback
 * to pace subsequent commits. */
static void commit_buffer(void) {
	if (g_gl_mode) {
		g_tex_dirty = 1;
		return;
	}
	if (g_shm_active < 0) return;
	struct shm_pool_buf *b = &g_shm_pool[g_shm_active];
	wl_surface_attach(g_surface, b->wl_buf, 0, 0);
	wl_surface_damage_buffer(g_surface, 0, 0, g_surf_w, g_surf_h);

	/* Register a fresh frame callback BEFORE commit so the request is part
	 * of the same commit transaction. */
	if (g_overlay_frame_cb) {
		/* Defensive: shouldn't happen if the gate at the tick site holds,
		 * but if it does, drop the stale callback rather than leak it. */
		wl_callback_destroy(g_overlay_frame_cb);
		g_overlay_frame_cb = NULL;
	}
	g_overlay_frame_cb = wl_surface_frame(g_surface);
	wl_callback_add_listener(g_overlay_frame_cb, &g_overlay_frame_listener, NULL);
	g_overlay_frame_pending = 1;

	wl_surface_commit(g_surface);
	b->busy = 1; // compositor now owns it until wl_buffer.release
}

static int has_content_to_show(void) {
	if (g_active_transient[0]) return 1;
	if (g_view_text[0]) return 1;
	/* In the middle of a fade-out from a view (text cleared but alpha hasn't
	 * hit 0 yet). Keep the surface alive so the fade can actually play out -
	 * otherwise destroy would snap to invisible. */
	if (g_visible_phase == (int)OVERLAY_PHASE_FADE_OUT && g_visible_alpha > 0.001f)
		return 1;
	return 0;
}

/* Config slice. Overlay owns its slice storage. Config routes
 * "overlay.*" keys here and overlay applies the slice to live state
 * (`g_style` + burn and fade targets) at init and on reload. Peers should
 * read values via the accessors, not the slice. */
static struct overlay_config s_cfg;

/* Parse the layer config value: 0=BACK (in-scene GL), 1=FRONT (Wayland
 * LAYER_OVERLAY). Rejects other values (prevents NULL deref on layer-shell
 * when not bound). */
static int parse_layer_bool(const char *key, const char *val) {
	int n = atoi(val);
	if (n == 0 || n == 1) return n;
	fprintf(stderr, "[config] %s=%s out of range (valid: 0=BACK, 1=FRONT); "
			"using 0 (BACK)\n", key, val);
	return 0;
}

static void overlay_config_defaults(void) {
	overlay_style_defaults(&s_cfg.style);
	s_cfg.burn = 1;
	s_cfg.burn_ms = 1500;
	s_cfg.fade_in_ms = 1000;
	s_cfg.fade_out_ms = 1000;
	s_cfg.flash = 1;
}

/* subkey is the part after "overlay." - return 1 if recognized, else 0. */
static int overlay_config_parse(const char *k, const char *val) {
	struct overlay_style *s = &s_cfg.style;

	if (!strcmp(k, "font")) snprintf(s->font_family, sizeof(s->font_family), "%s", val);
	else if (!strcmp(k, "font_size")) s->font_size = atof(val);
	else if (!strcmp(k, "color")) color_parse_hex(val, &s->color_r, &s->color_g, &s->color_b, &s->color_a);
	else if (!strcmp(k, "stroke")) color_parse_hex(val, &s->stroke_r, &s->stroke_g, &s->stroke_b, &s->stroke_a);
	else if (!strcmp(k, "stroke_width")) s->stroke_width = atof(val);
	else if (!strcmp(k, "w")) coord_length_parse(val, &s->size_w);
	else if (!strcmp(k, "h")) coord_length_parse(val, &s->size_h);
	else if (!strcmp(k, "x")) coord_length_parse(val, &s->pos_x);
	else if (!strcmp(k, "y")) coord_length_parse(val, &s->pos_y);
	else if (!strcmp(k, "anchor")) coord_anchor_parse(val, &s->anchor);
	else if (!strcmp(k, "art_placement")) s->art_placement = atoi(val);
	else if (!strcmp(k, "art_size")) s->art_size_frac = atof(val);
	else if (!strcmp(k, "art_alpha")) s->art_alpha = (float)atof(val);
	else if (!strcmp(k, "layer")) s->layer = parse_layer_bool("overlay.layer", val);
	else if (!strcmp(k, "transient_duration")) s->transient_duration = atoi(val);
	else if (!strcmp(k, "info_duration")) s->info_duration = atoi(val);
	else if (!strcmp(k, "burn")) s_cfg.burn = atoi(val);
	else if (!strcmp(k, "burn_ms")) s_cfg.burn_ms = atoi(val);
	else if (!strcmp(k, "fade_in_ms")) s_cfg.fade_in_ms = atoi(val);
	else if (!strcmp(k, "fade_out_ms")) s_cfg.fade_out_ms = atoi(val);
	else if (!strcmp(k, "flash")) s_cfg.flash = atoi(val);
	else return 0;
	return 1;
}

void overlay_config_apply(void) {
	overlay_update_style(&s_cfg.style); // staged for next tick
	overlay_set_burn_ms(s_cfg.burn_ms);
	overlay_set_fade_durations(s_cfg.fade_in_ms, s_cfg.fade_out_ms);
}

void overlay_clamp_layer_to_back(const char *mode_name) {
	if (s_cfg.style.layer != SCENE_LAYER_BACK) {
		fprintf(stderr, "[overlay] %s mode: forcing layer=0 (BACK), was %d\n",
				mode_name ? mode_name : "?", s_cfg.style.layer);
		s_cfg.style.layer = SCENE_LAYER_BACK;
	}
}

int overlay_burn_enabled(void) { return s_cfg.burn; }

bool overlay_spec_parse(int argc, char **argv, struct overlay_spec *d,
                           char *err, size_t errlen) {
	overlay_spec_init(d);

	int toff = 0; // write cursor into d->text
	int n_text = 0;

	#define OL_ERR(fmt, ...) do { \
		snprintf(err, errlen, fmt, ##__VA_ARGS__); \
		return false; \
	} while (0)

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

		#define OL_VAL(flag) do { \
			if (!v) OL_ERR("%s needs a value", flag); \
			i++; \
		} while (0)

		if (!strcmp(a, "-t") || !strcmp(a, "--text")) {
			OL_VAL("--text");
			if (n_text > 0 && toff < (int)sizeof(d->text) - 1)
				d->text[toff++] = '\n';
			int room = (int)sizeof(d->text) - toff;
			if (room > 1) {
				int w = snprintf(d->text + toff, (size_t)room, "%s", v);
				toff += (w >= 0 && w < room) ? w : room - 1;
			}
			d->text[toff] = '\0';
			n_text++;
		}
		else if (!strcmp(a, "-i") || !strcmp(a, "--image")) {
			OL_VAL("--image");
			snprintf(d->image_path, sizeof(d->image_path), "%s", v);
		}
		else if (!strcmp(a, "-du") || !strcmp(a, "--duration")) {
			OL_VAL("--duration"); d->duration_ms = atoi(v);
		}
		else if (!strcmp(a, "-fi") || !strcmp(a, "--fade-in")) {
			OL_VAL("--fade-in"); d->fade_in_ms = atoi(v);
		}
		else if (!strcmp(a, "-fo") || !strcmp(a, "--fade-out")) {
			OL_VAL("--fade-out"); d->fade_out_ms = atoi(v);
		}
		else if (!strcmp(a, "-b") || !strcmp(a, "--burn")) {
			OL_VAL("--burn"); d->burn = atoi(v) ? 1 : 0;
		}
		else if (!strcmp(a, "--art")) {
			OL_VAL("--art"); d->show_art = atoi(v) ? 1 : 0;
		}
		else if (!strcmp(a, "-aa") || !strcmp(a, "--art-alpha")) {
			OL_VAL("--art-alpha"); d->art_alpha = (float)atof(v);
		}
		else if (!strcmp(a, "-as") || !strcmp(a, "--art-size")) {
			OL_VAL("--art-size"); d->art_size_frac = (float)atof(v);
		}
		else if (!strcmp(a, "-ap") || !strcmp(a, "--art-pos")) {
			OL_VAL("--art-pos");
			if (!strcasecmp(v, "behind")) d->art_placement = OVERLAY_ART_BEHIND;
			else if (!strcasecmp(v, "left")) d->art_placement = OVERLAY_ART_LEFT;
			else if (!strcasecmp(v, "right")) d->art_placement = OVERLAY_ART_RIGHT;
			else if (!strcasecmp(v, "above")) d->art_placement = OVERLAY_ART_ABOVE;
			else if (!strcasecmp(v, "below")) d->art_placement = OVERLAY_ART_BELOW;
			else d->art_placement = atoi(v); // numeric fallback
		}
		else if (!strcmp(a, "-tc") || !strcmp(a, "--text-color")) {
			OL_VAL("--text-color");
			snprintf(d->text_color, sizeof(d->text_color), "%s", v);
		}
		else if (!strcmp(a, "-ts") || !strcmp(a, "--text-size")) {
			OL_VAL("--text-size"); d->text_size = atof(v);
		}
		else if (!strcmp(a, "-sc") || !strcmp(a, "--stroke-color")) {
			OL_VAL("--stroke-color");
			snprintf(d->stroke_color, sizeof(d->stroke_color), "%s", v);
		}
		else if (!strcmp(a, "-sw") || !strcmp(a, "--stroke-width")) {
			OL_VAL("--stroke-width"); d->stroke_width = atof(v);
		}
		else if (!strcmp(a, "-f") || !strcmp(a, "--font")) {
			OL_VAL("--font");
			snprintf(d->font, sizeof(d->font), "%s", v);
		}
		else if (!strcmp(a, "-x")) { OL_VAL("-x"); coord_length_parse(v, &d->pos_x); }
		else if (!strcmp(a, "-y")) { OL_VAL("-y"); coord_length_parse(v, &d->pos_y); }
		else if (!strcmp(a, "-w") || !strcmp(a, "--width")) {
			OL_VAL("--width"); coord_length_parse(v, &d->size_w);
		}
		else if (!strcmp(a, "-h") || !strcmp(a, "--height")) {
			OL_VAL("--height"); coord_length_parse(v, &d->size_h);
		}
		else if (!strcmp(a, "-a") || !strcmp(a, "--anchor")) {
			OL_VAL("--anchor");
			coord_anchor_t an;
			if (coord_anchor_parse(v, &an) == 0) d->anchor = (int)an;
			else OL_ERR("bad anchor: %s", v);
		}
		else if (!strcmp(a, "-l") || !strcmp(a, "--layer")) {
			OL_VAL("--layer");
			if (!strcmp(v, "back")) d->layer = SCENE_LAYER_BACK;
			else if (!strcmp(v, "front")) d->layer = SCENE_LAYER_FRONT;
			else d->layer = atoi(v);
		}
		else {
			OL_ERR("unknown flag: %s", a);
		}
		#undef OL_VAL
	}
	#undef OL_ERR

	return true;
}

static int overlay_ipc_show(struct ipc_command_ctx *c) {
	struct overlay_spec d;
	char err[192];

	if (!overlay_spec_parse(c->argc, c->argv, &d, err, sizeof(err))) {
		snprintf(c->reply, c->reply_len, "err overlay: %s\n", err);
		module_emit_reply("overlay", c->reply);
		return 0;
	}

	overlay_show(&d);
	snprintf(c->reply, c->reply_len, "ok overlay\n");
	return 0;
}

/* The single gate for command-feedback messages. Every renderer's
 * `module_emit_reply()` funnels here with its raw reply line. Gated
 * by `overlay.flash`. `source` is threaded so a future config can
 * mute or redirect per module without touching call sites. Rich
 * overlays bypass this and draw themselves directly. */
static void overlay_message_sink(const char *source, const char *reply) {
	(void)source;
	if (!s_cfg.flash || !reply || !reply[0]) return;

	const char *msg = reply;
	if (!strncmp(msg, "ok ", 3)) msg += 3;
	else if (!strncmp(msg, "ok\n", 3)) return; // bare ok - nothing to show
	else if (!strncmp(msg, "err ", 4)) msg += 4;

	char clean[256];
	snprintf(clean, sizeof(clean), "%.*s", (int)sizeof(clean) - 1, msg);
	size_t n = strlen(clean);
	if (n > 0 && clean[n-1] == '\n') clean[n-1] = '\0';
	if (clean[0]) overlay_push_transient(clean, 0);
}

void overlay_style_defaults(struct overlay_style *s) {
	snprintf(s->font_family, sizeof(s->font_family), "sans-serif");
	s->font_size = 36.0;
	s->color_r = 0.8f; s->color_g = 0.0f;
	s->color_b = 0.8f; s->color_a = 1.0f;
	s->stroke_r = 0.0f; s->stroke_g = 0.0f;
	s->stroke_b = 0.0f; s->stroke_a = 1.0f;
	s->stroke_width = 5.0;

	/* Full-width banner, half-height, centered - big and bold */
	s->size_w = coord_length_frac(1.0);
	s->size_h = coord_length_frac(0.5);
	s->pos_x = coord_length_frac(0.5);
	s->pos_y = coord_length_frac(0.5);
	s->anchor = COORD_ANCHOR_CENTER;

	s->art_placement = OVERLAY_ART_BEHIND;
	s->art_size_frac = 1.0;
	s->art_alpha = 0.7f;

	/* Defaults match the config-effective values. Overlay owns the slice. */
	s->layer = SCENE_LAYER_BACK;

	s->transient_duration = 2000;
	s->info_duration = 4000;
}

static void overlay_clear_overrides(void);

int overlay_init(struct wl_compositor *comp,
				 struct wl_shm *shm,
				 void *layer_shell,
				 struct wl_output *output,
				 int output_width, int output_height)
{
	g_comp = comp;
	g_shm = shm;
	g_layer_shell = (struct zwlr_layer_shell_v1 *)layer_shell;
	g_output = output;
	g_out_w = output_width;
	g_out_h = output_height;
	g_style = s_cfg.style;
	g_inited = 1;
	g_live = 0;
	g_dirty = 0;
	g_active_transient[0] = '\0';
	overlay_clear_overrides();
	overlay_set_burn_ms(s_cfg.burn_ms);
	overlay_set_fade_durations(s_cfg.fade_in_ms, s_cfg.fade_out_ms);
	return 1;
}

void overlay_update_style(const struct overlay_style *style) {
	pthread_mutex_lock(&g_req_lock);
	g_req_style = *style;
	g_req_style_pending = 1;
	pthread_mutex_unlock(&g_req_lock);
}

void overlay_set_output_size(int w, int h) {
	if (w > 0) g_out_w = w;
	if (h > 0) g_out_h = h;
}

void overlay_set_view(const char *text, int show_art) {
	pthread_mutex_lock(&g_req_lock);
	snprintf(g_req_view_text, VIEW_TEXT_MAX, "%s", text ? text : "");
	g_req_view_show_art = show_art ? 1 : 0;
	g_req_view_dirty = 1;
	pthread_mutex_unlock(&g_req_lock);
}

void overlay_set_art(const char *art_path) {
	pthread_mutex_lock(&g_req_lock);
	snprintf(g_req_art_path, sizeof(g_req_art_path), "%s", art_path ? art_path : "");
	g_req_art_dirty = 1;
	pthread_mutex_unlock(&g_req_lock);
}

void overlay_push_transient(const char *text, int duration_ms) {
	if (!text || !text[0]) return;
	pthread_mutex_lock(&g_req_lock);
	snprintf(g_req_transient_text, sizeof(g_req_transient_text), "%s", text);
	g_req_transient_duration = duration_ms;
	g_req_transient_pending = 1;
	pthread_mutex_unlock(&g_req_lock);
}

/* Compatibility shim - info-peek is now the first internal output of the
 * overlay pipeline. Build an overlay with sentinel defaults and the given
 * show_art flag. Everything else (duration, fades, art placement, alpha, size,
 * burn) falls back to style. Internal callers (the info-peek path,
 * now-playing flash) that haven't migrated yet keep this entry. */
void overlay_show_info(const char *text, int show_art) {
	struct overlay_spec d;
	overlay_spec_init(&d);
	snprintf(d.text, sizeof(d.text), "%s",
			 (text && text[0]) ? text : "(empty)");
	d.show_art = show_art ? 1 : 0;
	/* duration_ms left as sentinel -> apply block uses info_duration
	 * (the historical info-peek timing). */
	overlay_show(&d);
}

void overlay_show(const struct overlay_spec *d) {
	if (!d) return;
	pthread_mutex_lock(&g_req_lock);
	g_req_overlay = *d; // full copy - string fields are inline arrays
	/* Defensive null-terminate (caller might not have used the init helper) */
	g_req_overlay.text[sizeof(g_req_overlay.text) - 1] = '\0';
	g_req_overlay.image_path[sizeof(g_req_overlay.image_path) - 1] = '\0';
	g_req_overlay_pending = 1;
	pthread_mutex_unlock(&g_req_lock);
}

/* Lazy-init the burn shader program + VAO. Idempotent.
 * Vertex: fullscreen quad via gl_VertexID with Y-flip in UV math.
 * Fragment: BGRA->RGBA swizzle (.bgra) + alpha uniform multiply.
 * Output is premultiplied (Cairo source is premultiplied). */
static int overlay_burn_shader_init(void) {
	if (g_burn_prog) return 1;

	/* GLES 3.0 minimum (matches rest of daemon). Fragment precision
	 * highp for the multiply - mediump would clip the alpha
	 * resolution at the tail of the bell curve. */
	static const char *vs_src =
		"#version 300 es\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    vec2 p = vec2(\n"
		"        (gl_VertexID == 1 || gl_VertexID == 2) ?  1.0 : -1.0,\n"
		"        (gl_VertexID == 2 || gl_VertexID == 3) ?  1.0 : -1.0);\n"
		"    gl_Position = vec4(p, 0.0, 1.0);\n"
		/*    Flip Y for UV so the scratch ends up oriented to match
		 *    the visualizer's expected sampling (visual top at sampled
		 *    UV.y=1). NDC.y=-1 (scratch bottom) -> UV.y=1, NDC.y=+1
		 *    (scratch top) -> UV.y=0. */
		"    v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\n"
		"}\n";

	static const char *fs_src =
		"#version 300 es\n"
		"precision highp float;\n"
		"uniform sampler2D u_tex;\n"
		"uniform float     u_alpha;\n"
		"in  vec2 v_uv;\n"
		"out vec4 fragColor;\n"
		"void main() {\n"
		/*    Texture storage is Cairo little-endian (B,G,R,A) read
		 *    through GL_RGBA, so sampled.r is actually Blue, etc.
		 *    .bgra reorders to (R,G,B,A) which is what we want to
		 *    feed to the visualizer's burn blend. */
		"    vec4 sampled = texture(u_tex, v_uv);\n"
		"    fragColor    = sampled.bgra * u_alpha;\n"
		"}\n";

	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vs_src, NULL);
	glCompileShader(vs);
	GLint ok = 0;
	glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048]; GLsizei n = 0;
		glGetShaderInfoLog(vs, sizeof(log), &n, log);
		fprintf(stderr, "[burn] vertex shader compile failed: %.*s\n",
				(int)n, log);
		glDeleteShader(vs);
		return 0;
	}

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fs_src, NULL);
	glCompileShader(fs);
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048]; GLsizei n = 0;
		glGetShaderInfoLog(fs, sizeof(log), &n, log);
		fprintf(stderr, "[burn] fragment shader compile failed: %.*s\n",
				(int)n, log);
		glDeleteShader(vs);
		glDeleteShader(fs);
		return 0;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048]; GLsizei n = 0;
		glGetProgramInfoLog(prog, sizeof(log), &n, log);
		fprintf(stderr, "[burn] program link failed: %.*s\n",
				(int)n, log);
		glDeleteProgram(prog);
		return 0;
	}

	g_burn_prog = prog;
	g_burn_prog_tex_loc = glGetUniformLocation(prog, "u_tex");
	g_burn_prog_alpha_loc = glGetUniformLocation(prog, "u_alpha");

	/* Bound VAO required by GLES core profile even though the shader
	 * fetches positions from gl_VertexID with no vertex buffer. */
	glGenVertexArrays(1, &g_burn_vao);

	return 1;
}

/* Snapshot `g_pixels` into `g_burn_tex` and arm the wall-clock burn timeline.
 * Works in both GL and Wayland modes - `g_pixels` layout is identical.
 * No CPU swizzle or flip - burn shader handles BGRA->RGBA + Y-flip + alpha. */
static void overlay_snapshot_burn(void) {
	/* Lazy-init the burn shader on first call. If compile or link fails
	 * we can't run any burns at all - bail with the same FAILED log
	 * as a missing-pixels condition. */
	if (!overlay_burn_shader_init()) {
		fprintf(stderr,
			"[burn] fade-out start: snapshot FAILED (shader init failed)\n");
		return;
	}

	if (g_burn_tex && (g_burn_tex_w != g_surf_w || g_burn_tex_h != g_surf_h)) {
		glDeleteTextures(1, &g_burn_tex);
		g_burn_tex = 0;
	}
	if (!g_burn_tex) {
		glGenTextures(1, &g_burn_tex);
		if (g_burn_tex) {
			glBindTexture(GL_TEXTURE_2D, g_burn_tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
				         g_surf_w, g_surf_h, 0,
				         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, 0);
			g_burn_tex_w = g_surf_w;
			g_burn_tex_h = g_surf_h;
		}
	}

	/* (re)allocate scratch FBO+texture for the per-frame shader pass. The
	 * shader writes color * alpha into this texture */
	if (g_burn_scratch_tex && (g_burn_scratch_w != g_surf_w
				            || g_burn_scratch_h != g_surf_h)) {
		glDeleteFramebuffers(1, &g_burn_scratch_fbo);
		glDeleteTextures(1, &g_burn_scratch_tex);
		g_burn_scratch_fbo = 0;
		g_burn_scratch_tex = 0;
	}
	if (!g_burn_scratch_tex) {
		glGenTextures(1, &g_burn_scratch_tex);
		if (g_burn_scratch_tex) {
			glBindTexture(GL_TEXTURE_2D, g_burn_scratch_tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
				         g_surf_w, g_surf_h, 0,
				         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, 0);
			g_burn_scratch_w = g_surf_w;
			g_burn_scratch_h = g_surf_h;

			glGenFramebuffers(1, &g_burn_scratch_fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, g_burn_scratch_fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				                   GL_TEXTURE_2D, g_burn_scratch_tex, 0);
			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				fprintf(stderr,
				    "[burn] scratch FBO incomplete (0x%x) — burn disabled\n",
				    (unsigned)status);
				glDeleteFramebuffers(1, &g_burn_scratch_fbo);
				glDeleteTextures(1, &g_burn_scratch_tex);
				g_burn_scratch_fbo = 0;
				g_burn_scratch_tex = 0;
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	/* Direct upload: `g_pixels` (Cairo little-endian B,G,R,A bytes,
	 * top-down) -> `g_burn_tex` via glTexSubImage2D. No CPU work - the
	 * burn shader handles both the BGRA->RGBA swizzle (via .bgra in
	 * the FS) and the Y-flip (via UV math in the VS). This avoids a
	 * CPU loop that at 4K cost ~20-30ms single-core ARM on every snapshot. */
	if (g_burn_tex && g_burn_scratch_tex && g_pixels) {
		glBindTexture(GL_TEXTURE_2D, g_burn_tex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
				        g_surf_w, g_surf_h,
				        GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
		glBindTexture(GL_TEXTURE_2D, 0);

		g_burn_rect_x = g_screen_x;
		g_burn_rect_y = g_screen_y;
		g_burn_rect_w = g_surf_w;
		g_burn_rect_h = g_surf_h;

		/* Record wall-clock snapshot time. The poll path computes
		 * elapsed ms from this to derive the burn phase - completely
		 * rate-independent, no frame counters or vrefresh needed. */
		clock_gettime(CLOCK_MONOTONIC, &g_burn_start_time);
		g_burn_active = 1;

		fprintf(stderr,
			"[burn] fade-out start: snapshot taken (tex=%u scratch=%u "
			"rect=%d,%d %dx%d duration=%dms)\n",
			(unsigned)g_burn_tex, (unsigned)g_burn_scratch_tex,
			g_burn_rect_x, g_burn_rect_y,
			g_burn_rect_w, g_burn_rect_h,
			g_burn_ms_target);
	} else {
		fprintf(stderr,
			"[burn] fade-out start: snapshot FAILED (burn_tex=%u "
			"scratch=%u pixels=%p)\n",
			(unsigned)g_burn_tex,
			(unsigned)g_burn_scratch_tex, (void*)g_pixels);
	}
}

/* Requests posted by the setters, drained under lock once per tick. Each
 * `*_dirty` gate carries its own payload. */
struct overlay_request {
	int view_dirty;
	char view_text[VIEW_TEXT_MAX];
	int view_show_art;

	int art_dirty;
	char art_path[1024];

	int transient_pending;
	char transient_text[2048];
	int transient_duration;

	int overlay_pending;
	struct overlay_spec overlay;

	int style_pending;
	struct overlay_style style;
};

static void overlay_drain_requests(struct overlay_request *r) {
	memset(r, 0, sizeof(*r));

	pthread_mutex_lock(&g_req_lock);
	if (g_req_view_dirty) {
		r->view_dirty = 1;
		memcpy(r->view_text, g_req_view_text, VIEW_TEXT_MAX);
		r->view_show_art = g_req_view_show_art;
		g_req_view_dirty = 0;
	}
	if (g_req_art_dirty) {
		r->art_dirty = 1;
		snprintf(r->art_path, sizeof(r->art_path), "%s", g_req_art_path);
		g_req_art_dirty = 0;
	}
	if (g_req_transient_pending) {
		r->transient_pending = 1;
		snprintf(r->transient_text, sizeof(r->transient_text), "%s",
		         g_req_transient_text);
		r->transient_duration = g_req_transient_duration;
		g_req_transient_pending = 0;
	}
	if (g_req_overlay_pending) {
		r->overlay_pending = 1;
		r->overlay = g_req_overlay;
		g_req_overlay_pending = 0;
	}
	if (g_req_style_pending) {
		r->style_pending = 1;
		r->style = g_req_style;
		g_req_style_pending = 0;
	}
	pthread_mutex_unlock(&g_req_lock);
}

/* Is another request queued behind the one now fading out, or is the composed
 * view still up? Either way the overlay stack has not drained, so no burn.
 * `count_view` is 0 on the view's own fade-out, where the text is already
 * known clear. */
static int overlay_stack_continues(int count_view) {
	int queued = 0;

	pthread_mutex_lock(&g_req_lock);
	if (g_req_transient_pending || g_req_overlay_pending || g_req_view_dirty)
		queued = 1;
	pthread_mutex_unlock(&g_req_lock);

	if (!queued && count_view && g_view_text[0]) queued = 1;
	return queued;
}

/* Resolve the surface geometry from style plus any live overlay overrides.
 * A changed size or layer mode can't be adjusted in place, so tear the
 * surface down and let the lifecycle stand the new one up. Otherwise the
 * position alone moves. Constant geometry across frames costs nothing. */
static void overlay_apply_geometry(void) {
	int prev_w = g_surf_w;
	int prev_h = g_surf_h;
	int prev_mode_is_gl = g_gl_mode;

	int eff_layer = (g_active_layer != OVERLAY_SPEC_DEFAULT)
	                ? g_active_layer : g_style.layer;
	int new_mode_is_gl = (eff_layer == SCENE_LAYER_BACK);

	compute_geometry();

	int size_changed = (g_surf_w != prev_w || g_surf_h != prev_h);
	int mode_changed = (g_live && new_mode_is_gl != prev_mode_is_gl);

	if (g_live && (size_changed || mode_changed)) destroy_surface();
	else compute_position();
}

/* Return every per-overlay override to its sentinel and release the overlay's
 * own art, so the next thing on screen inherits nothing from it. */
static void overlay_clear_overrides(void) {
	if (g_active_overlay_art) {
		cairo_surface_destroy(g_active_overlay_art);
		g_active_overlay_art = NULL;
	}
	g_active_art_placement = OVERLAY_SPEC_DEFAULT;
	g_active_art_alpha = (float)OVERLAY_SPEC_DEFAULT;
	g_active_art_size_frac = (float)OVERLAY_SPEC_DEFAULT;
	g_active_burn = OVERLAY_SPEC_DEFAULT;
	g_active_pos_x = coord_length_unspecified();
	g_active_pos_y = coord_length_unspecified();
	g_active_size_w = coord_length_unspecified();
	g_active_size_h = coord_length_unspecified();
	g_active_anchor = OVERLAY_SPEC_DEFAULT;
	g_active_layer = OVERLAY_SPEC_DEFAULT;
}

/* A geometry change can't be applied to a live surface, so drop it and let
 * the lifecycle rebuild. */
static void overlay_apply_style(const struct overlay_style *s) {
	#define LEN_EQ(a, b) ((a).present == (b).present \
	                      && (a).unit == (b).unit \
	                      && (a).value == (b).value)
	int geom_changed =
		!LEN_EQ(s->size_w, g_style.size_w) ||
		!LEN_EQ(s->size_h, g_style.size_h) ||
		!LEN_EQ(s->pos_x, g_style.pos_x) ||
		!LEN_EQ(s->pos_y, g_style.pos_y) ||
		s->anchor != g_style.anchor ||
		s->layer != g_style.layer;
	#undef LEN_EQ

	g_style = *s;
	if (geom_changed && g_live) destroy_surface();
	g_dirty = 1;
}

static void overlay_apply_view(const char *text, int show_art) {
	snprintf(g_view_text, VIEW_TEXT_MAX, "%.*s", VIEW_TEXT_MAX - 1, text);
	g_view_show_art = show_art;
	g_dirty = 1;
}

/* Decode and cache the now-playing art. Empty path clears it.
 *
 * Cached by path AND mtime: remote art is rewritten to the same /tmp filename
 * for every track, so the path alone is not a key. The attempt is recorded
 * even when the decode fails, so a persistently bad path isn't re-decoded
 * every pass. A later mtime change still retries. A failed load clears the
 * surface rather than leaving the previous track's art up. */
static void overlay_apply_pending_art(const char *path) {
	g_dirty = 1;

	if (!path[0]) {
		if (g_art_surface) {
			cairo_surface_destroy(g_art_surface);
			g_art_surface = NULL;
		}
		g_art_loaded_path[0] = '\0';
		g_art_loaded_mtime = 0;
		return;
	}

	struct stat st;
	int have_stat = (stat(path, &st) == 0);
	int path_diff = strcmp(path, g_art_loaded_path) != 0;
	int mtime_diff = have_stat && st.st_mtime != g_art_loaded_mtime;
	if (!path_diff && !mtime_diff) return;

	art_decode_info ai;
	cairo_surface_t *neu = art_decode_image(path, &ai);
	if (!neu) {
		if (ai.status == ART_DECODE_BAD_DIMENSIONS)
			fprintf(stderr, "[overlay] art rejected: %dx%d over limit: %s\n",
			        ai.width, ai.height, path);
		else
			fprintf(stderr, "[overlay] art decode failed: %s\n", path);
	}

	if (g_art_surface) cairo_surface_destroy(g_art_surface);
	g_art_surface = neu;
	snprintf(g_art_loaded_path, sizeof(g_art_loaded_path), "%s", path);
	g_art_loaded_mtime = have_stat ? st.st_mtime : 0;
}

/* Seat an overlay as the transient on screen. Its own image decodes here
 * (synchronously, failure is non-fatal), its overrides land in the g_active_*
 * sentinels, and it supersedes anything already showing. */
static void overlay_apply_overlay(const struct overlay_spec *d) {
	cairo_surface_t *new_art = NULL;
	if (d->image_path[0]) {
		new_art = art_decode_image(d->image_path, NULL);
		if (!new_art)
			fprintf(stderr, "[overlay] image decode failed: %s\n",
			        d->image_path);
	}

	overlay_clear_overrides();
	g_active_overlay_art = new_art; // may be NULL

	/* Empty text is valid (image-only overlay). A single space keeps the
	 * composer from short-circuiting on an empty string. */
	snprintf(g_active_transient, sizeof(g_active_transient), "%s",
	         d->text[0] ? d->text : " ");

	g_active_transient_duration = (d->duration_ms > 0)
		? d->duration_ms : g_style.info_duration;
	g_fade_in_ms_active = (d->fade_in_ms >= 0)
		? d->fade_in_ms : g_fade_in_ms_target;
	g_fade_out_ms_active = (d->fade_out_ms >= 0)
		? d->fade_out_ms : g_fade_out_ms_target;

	clock_gettime(CLOCK_MONOTONIC, &g_active_transient_start);

	/* Art shows on request, and implicitly whenever the overlay brought
	 * its own image. */
	g_active_show_art = d->show_art ? 1 : 0;
	if (g_active_overlay_art) g_active_show_art = 1;

	if (d->art_placement >= 0) g_active_art_placement = d->art_placement;
	if (d->art_alpha >= 0.0f) g_active_art_alpha = d->art_alpha;
	if (d->art_size_frac > 0.0f) g_active_art_size_frac = d->art_size_frac;
	if (d->burn >= 0) g_active_burn = d->burn;

	if (d->pos_x.present) g_active_pos_x = d->pos_x;
	if (d->pos_y.present) g_active_pos_y = d->pos_y;
	if (d->size_w.present) g_active_size_w = d->size_w;
	if (d->size_h.present) g_active_size_h = d->size_h;
	if (d->anchor != OVERLAY_SPEC_DEFAULT) g_active_anchor = d->anchor;
	if (d->layer == SCENE_LAYER_BACK || d->layer == SCENE_LAYER_FRONT)
		g_active_layer = d->layer;

	overlay_apply_geometry();
	resolve_text_style_overlay(d);

	g_visible_phase = OVERLAY_PHASE_NONE;
	g_view_was_visible = 0;
	if (g_burn_active) {
		g_burn_active = 0;
		fprintf(stderr, "[burn] cancelled: superseded by new overlay\n");
	}
	g_dirty = 1;
}

/* Seat a plain transient. Carries no overrides and no art, so the overlay
 * that may have preceded it is cleared out first. */
static void overlay_apply_transient(const char *text, int duration_ms) {
	snprintf(g_active_transient, sizeof(g_active_transient), "%s", text);
	g_active_transient_duration = duration_ms > 0
		? duration_ms : g_style.transient_duration;
	clock_gettime(CLOCK_MONOTONIC, &g_active_transient_start);
	g_active_show_art = 0;

	g_fade_in_ms_active = g_fade_in_ms_target;
	g_fade_out_ms_active = g_fade_out_ms_target;

	overlay_clear_overrides();
	overlay_apply_geometry();
	resolve_text_style_base();

	g_visible_phase = OVERLAY_PHASE_NONE;
	g_view_was_visible = 0;
	if (g_burn_active) {
		g_burn_active = 0;
		fprintf(stderr, "[burn] cancelled: superseded by new transient\n");
	}
	g_dirty = 1;
}

/* Advance the transient's fade. On FADE_OUT entry the burn arms, but only on
 * a true exit: nothing queued behind it, no composed view underneath, and the
 * overlay didn't opt out. Alpha is a shader uniform, so no re-render here. */
static void overlay_drive_fade(void) {
	if (!fade_tick_advance()) return;

	if (overlay_stack_continues(1)) {
		fprintf(stderr, "[burn] FADE_OUT entered but stack not draining — burn suppressed\n");
	} else if (g_active_burn == 0) {
		fprintf(stderr, "[burn] FADE_OUT entered but overlay burn=0 — burn suppressed\n");
	} else if (g_pixels && g_surf_w > 0 && g_surf_h > 0) {
		overlay_snapshot_burn();
	} else {
		fprintf(stderr, "[burn] FADE_OUT entered but burn NOT armed: gl_mode=%d gl_tex=%u surf=%dx%d\n",
		        g_gl_mode, (unsigned)g_gl_tex, g_surf_w, g_surf_h);
	}
}

/* Retire the transient once its wall-clock duration is spent, leaving the
 * overlay idle and any composed view free to fade back in. Returns 1 if
 * it retired on this tick. */
static int overlay_teardown_faded(void) {
	if (!g_active_transient[0]) return 0;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double from_start_ms =
		(now.tv_sec - g_active_transient_start.tv_sec) * 1000.0 +
		(now.tv_nsec - g_active_transient_start.tv_nsec) / 1e6;
	if (from_start_ms < g_active_transient_duration) return 0;

	g_active_transient[0] = '\0';
	g_active_show_art = 0;
	g_visible_phase = OVERLAY_PHASE_NONE;
	g_visible_alpha = 0.0f; // picked up by view fade-in if applicable
	g_view_was_visible = 0; // view, if it appears now, is fresh

	overlay_clear_overrides();
	overlay_apply_geometry();

	g_dirty = 1;
	return 1;
}

/* Drive the composed view's fade in the gap between transients. It fades in
 * on appearance and out when cleared, and burns on a true exit the same way a
 * transient does - the view text is already clear by then, so only a queued
 * request can hold the burn off. */
static void overlay_drive_view_fade(int transient_just_ended) {
	int view_now_visible = g_view_text[0] ? 1 : 0;

	/* No transient has run yet, so the active fade durations are still
	 * unseeded. Take the configured targets. */
	if (g_fade_in_ms_active == 0 && g_fade_out_ms_active == 0) {
		g_fade_in_ms_active = g_fade_in_ms_target;
		g_fade_out_ms_active = g_fade_out_ms_target;
	}

	int view_fade_out = view_tick_advance(view_now_visible);

	if (view_fade_out) {
		if (overlay_stack_continues(0)) {
			fprintf(stderr, "[burn] view FADE_OUT but stack not draining — burn suppressed\n");
		} else if (g_pixels && g_surf_w > 0 && g_surf_h > 0) {
			overlay_snapshot_burn();
		} else {
			fprintf(stderr, "[burn] view FADE_OUT but burn NOT armed: gl_mode=%d gl_tex=%u\n",
			        g_gl_mode, (unsigned)g_gl_tex);
		}
	}

	/* A view appearing on its own already marked dirty when its text was
	 * applied. This covers the other way in: a transient retiring onto a
	 * view that was underneath it all along. */
	if (transient_just_ended) g_dirty = 1;
}

/* Surface lifecycle plus the paint:
 *   need_show && !live  -> create surface, wait for configure
 *   !need_show && live  -> destroy surface
 *   live && !configured -> wait
 *   live && configured && dirty -> render + commit */
static void overlay_repaint(void) {
	if (!has_content_to_show()) {
		if (g_live) destroy_surface();
		g_dirty = 0;
		return;
	}

	if (!g_live) {
		if (!create_surface()) return;
		g_dirty = 1; // will render once configured
		return;
	}

	if (!g_configured || !g_dirty) return;

	/* FRONT mode only: hold the paint if the compositor hasn't presented the
	 * last commit, leaving `g_dirty` set for the next eligible tick. GL mode
	 * renders through the scene composite instead. */
	if (!g_gl_mode && g_overlay_frame_pending) return;

	prepare_paint_target();
	if (g_active_transient[0])
		render_transient(g_active_transient, g_active_show_art);
	else
		render_view();
	commit_buffer();
	g_dirty = 0;
}

void overlay_tick(void) {
	if (!g_inited) return;

	struct overlay_request r;
	overlay_drain_requests(&r);

	if (r.style_pending) overlay_apply_style(&r.style);
	if (r.view_dirty) overlay_apply_view(r.view_text, r.view_show_art);
	if (r.art_dirty) overlay_apply_pending_art(r.art_path);

	/* Overlay and plain transient share the fade and lifecycle below. A
	 * overlay arriving on the same tick wins. */
	if (r.overlay_pending)
		overlay_apply_overlay(&r.overlay);
	else if (r.transient_pending)
		overlay_apply_transient(r.transient_text, r.transient_duration);

	overlay_drive_fade();

	int transient_just_ended = overlay_teardown_faded();
	if (!g_active_transient[0])
		overlay_drive_view_fade(transient_just_ended);

	/* Wayland mode bakes alpha into the pixels, and the multiply is
	 * destructive, so every fade tick has to re-paint from full strength.
	 * GL mode carries alpha in a shader uniform and needs no re-paint. */
	if (!g_gl_mode &&
	    (g_visible_phase == (int)OVERLAY_PHASE_FADE_IN ||
	     g_visible_phase == (int)OVERLAY_PHASE_FADE_OUT))
		g_dirty = 1;

	overlay_repaint();
}

void overlay_render_present(int output_w, int output_h) {
	if (!g_gl_mode || !g_live || !g_gl_tex || !g_pixels) return;

	if (g_tex_dirty) {
		glBindTexture(GL_TEXTURE_2D, g_gl_tex);
		/* Cairo wrote ARGB32 (BGRA in memory on little-endian). We swizzle to
		 * RGB inside the fragment shader, so just upload as RGBA. */
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_surf_w, g_surf_h,
		                GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
		glBindTexture(GL_TEXTURE_2D, 0);
		g_tex_dirty = 0;
	}

	/* `g_visible_alpha` is the fade curve for whatever is on screen, driven
	 * each tick by the transient or the view fade. */
	gl_quad_blit(g_gl_tex, g_screen_x, g_screen_y,
	  g_surf_w, g_surf_h, output_w, output_h, g_visible_alpha);
}

/* The pending flag is one-shot: cleared on return whether or not the caller
 * runs the burn. If the texture or geometry went invalid between arming and
 * this poll (overlay torn down in the gap), the flag clears and we report no
 * burn - the moment has passed. */
int overlay_poll_burn(uint32_t *out_tex, int *out_x, int *out_y, int *out_w,
                      int *out_h, float *out_alpha) {
	if (!g_burn_active)
		return 0;

	/* Validate. CPU snapshot buffers are gone (`g_pixels` uploads
	* directly), so the moving parts are: the uploaded snapshot
	* texture, the scratch FBO and texture, the burn shader, and a
	* non-degenerate ms_target. */
	if (!g_burn_tex || !g_burn_scratch_tex || !g_burn_scratch_fbo ||
	    !g_burn_prog || !g_burn_vao || g_burn_rect_w <= 0 ||
	    g_burn_rect_h <= 0 || g_burn_ms_target < 1) {
		g_burn_active = 0;
		return 0;
	}

	/* Wall-clock elapsed since snapshot. Completely rate-independent -
	* works correctly at any frame rate, survives mid-burn rate
	* changes, no frame counters needed. */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double elapsed_ms = (now.tv_sec - g_burn_start_time.tv_sec) * 1000.0 +
	                     (now.tv_nsec - g_burn_start_time.tv_nsec) / 1e6;

	if (elapsed_ms >= (double)g_burn_ms_target) {
		g_burn_active = 0;
		return 0;
	}

	/* Continuous per-frame alpha. Sine bell across the wall-clock
	* duration */
	float phase = (float)(elapsed_ms / (double)g_burn_ms_target);
	float t;
	if (g_burn_ms_target <= 1) {
		t = 1.0f;
	} else {
		t = sinf((float)M_PI * phase);
	}
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;

	/* Short-circuit at the tails of the bell. Below ~2% the rendered
	* burn contribution is psychovisually indistinguishable from
	* nothing. Skip the scratch render pass so the caller skips its
	* blend too - net zero work for these frames. */
	if (t < 0.02f) return 0;

	/* GPU pass: render `g_burn_tex` through burn shader into scratch with
	* alpha uniform `t`. Saves+restores FBO, viewport, blend state. */
	GLint prev_fbo, prev_vp[4], prev_prog;
	GLboolean prev_blend;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_VIEWPORT, prev_vp);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
	prev_blend = glIsEnabled(GL_BLEND);

	glBindFramebuffer(GL_FRAMEBUFFER, g_burn_scratch_fbo);
	glViewport(0, 0, g_burn_scratch_w, g_burn_scratch_h);
	glDisable(GL_BLEND);

	glUseProgram(g_burn_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_burn_tex);
	glUniform1i(g_burn_prog_tex_loc, 0);
	glUniform1f(g_burn_prog_alpha_loc, t);

	glBindVertexArray(g_burn_vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram((GLuint)prev_prog);
	
	if (prev_blend) glEnable(GL_BLEND);
	else glDisable(GL_BLEND);
	
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
	glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

	if (out_tex)
		*out_tex = (uint32_t)g_burn_scratch_tex;
	if (out_x)
		*out_x = g_burn_rect_x;
	if (out_y)
		*out_y = g_burn_rect_y;
	if (out_w)
		*out_w = g_burn_rect_w;
	if (out_h)
		*out_h = g_burn_rect_h;
	if (out_alpha)
		*out_alpha = t;
	return 1;
}

void overlay_set_burn_ms(int ms) {
	g_burn_ms_target = (ms < 1) ? 1 : ms;
}

void overlay_set_fade_durations(int fade_in_ms, int fade_out_ms) {
	g_fade_in_ms_target = (fade_in_ms < 0) ? 0 : fade_in_ms;
	g_fade_out_ms_target = (fade_out_ms < 0) ? 0 : fade_out_ms;
}

void overlay_destroy(void) {
	destroy_surface();
	if (g_art_surface) {
		cairo_surface_destroy(g_art_surface);
		g_art_surface = NULL;
	}
	if (g_active_overlay_art) {
		cairo_surface_destroy(g_active_overlay_art);
		g_active_overlay_art = NULL;
	}
	if (g_burn_tex) {
		glDeleteTextures(1, &g_burn_tex);
		g_burn_tex = 0;
		g_burn_tex_w = g_burn_tex_h = 0;
	}
	if (g_burn_scratch_fbo) {
		glDeleteFramebuffers(1, &g_burn_scratch_fbo);
		g_burn_scratch_fbo = 0;
	}
	if (g_burn_scratch_tex) {
		glDeleteTextures(1, &g_burn_scratch_tex);
		g_burn_scratch_tex = 0;
		g_burn_scratch_w = g_burn_scratch_h = 0;
	}
	if (g_burn_prog) {
		glDeleteProgram(g_burn_prog);
		g_burn_prog = 0;
		g_burn_prog_alpha_loc = -1;
		g_burn_prog_tex_loc = -1;
	}
	if (g_burn_vao) {
		glDeleteVertexArrays(1, &g_burn_vao);
		g_burn_vao = 0;
	}
	g_burn_active = 0;
	g_visible_phase = OVERLAY_PHASE_NONE;
	g_visible_alpha = 1.0f;
	g_inited = 0;
}

MODULE_REGISTER(overlay,
	.config_prefix = "overlay",
	.config_template =
		"overlay.font=sans-serif\n"
		"overlay.font_size=36   # pixels\n"
		"overlay.color=cc00ccff   # hex RGBA\n"
		"overlay.stroke=ffffffff   # hex RGBA\n"
		"overlay.stroke_width=5.0   # pixels\n"
		"overlay.w=1.0   # length. fractional default. '*px' to force pixels\n"
		"overlay.h=0.5   # length\n"
		"overlay.x=0.5   # length. anchor point on display\n"
		"overlay.y=0.5   # length\n"
		"overlay.anchor=center   # top-left|top|top-right|left|center|right|bottom-left|bottom|bottom-right\n"
		"overlay.layer=1   # bool. 0 = in scene, 1 = above all\n"
		"overlay.art_placement=0   # integer. behind, left, right, above, below\n"
		"overlay.art_size=1.0   # fraction\n"
		"overlay.art_alpha=0.7   # fraction\n"
		"overlay.transient_duration=2000   # milliseconds\n"
		"overlay.info_duration=4000   # milliseconds\n"
		"overlay.fade_in_ms=1500   # milliseconds\n"
		"overlay.fade_out_ms=1500   # milliseconds\n"
		"overlay.burn=1	  # boolean\n"
		"overlay.burn_ms=1500   # milliseconds. duration of exit burn\n"
		"overlay.flash=1   # boolean. feedback messages off/on\n",
	.config_defaults = overlay_config_defaults,
	.config_parse = overlay_config_parse,
	.config_apply = overlay_config_apply,
	.message_sink = overlay_message_sink,
	.ipc_verb = "overlay",
	.ipc_command = overlay_ipc_show,
	.ipc_help =
		"\noverlay — client-side flag grammar:\n"
		"  overlay [flags]\n"
		"    -t  --text LINE     one text line; repeat for multiple lines\n"
		"    -i  --image PATH    image to show (png/jpeg)\n"
		"    -du --duration MS   total time on screen (ms)\n"
		"    -fi --fade-in MS    -fo --fade-out MS\n"
		"    -b  --burn 0|1      burn-in contrail through the visualizer\n"
		"    -tc --text-color HEX 8-hex RRGGBBAA   -ts --text-size PX\n"
		"    -sc --stroke-color HEX                -sw --stroke-width PX\n"
		"    -f  --font FAMILY\n"
		"        --art 0|1       -aa --art-alpha F   -as --art-size F\n"
		"    -ap --art-pos behind|left|right|above|below\n"
		"    Placement (coord language: bare = fraction, '100px' = pixels):\n"
		"    -x L -y L              anchor point on display\n"
		"    -w L -h L              bounding rect size\n"
		"    -a --anchor NAME       top-left|top|top-right|left|center|right|\n"
		"                           bottom-left|bottom|bottom-right  (default: center)\n"
		"    -l --layer back|front  override config overlay_layer for this overlay\n"
		"    Quote multiword lines:  overlay -t \"$(date)\" -t \"white rabbit\"\n");
