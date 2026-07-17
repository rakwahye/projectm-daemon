// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file visualizer_projectm.c
 * @brief libprojectM preset engine.
 *
 * Holds one projectM handle. Loads presets, feeds it PCM, renders a
 * frame per call. Caches the last fps and mesh size pushed and drops
 * identical re-pushes, since libprojectM treats a repeat as a real
 * config change and rebuilds internal state. */

#include "render_params.h"
#include "presets.h"
#include "visualizer.h"
#include "module_registry.h"
#include "util.h"
#include "playlist.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <GLES3/gl3.h>
#include <projectM-4/projectM.h>

/* The logging API (projectm_set_log_callback etc.) exists only on
 * libprojectM git master. No 4.x release ships logging.h. */
#if __has_include(<projectM-4/logging.h>)
# include <projectM-4/logging.h>
# define PM_HAS_LOGGING 1
#else
# define PM_HAS_LOGGING 0
#endif

/* projectm_opengl_burn_texture is declared in render_opengl.h, but only
 * on git master. Every 4.x release ships render_opengl.h WITHOUT the
 * function. */
#if __has_include(<projectM-4/render_opengl.h>)
# include <projectM-4/render_opengl.h>
#endif
#define PM_HAS_BURN PM_HAS_LOGGING

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while (0)

/* Config slice. */
struct pm_config {
	char texture_dir[512];
	double soft_cut_duration;
	int hard_cut_enabled;
	double hard_cut_duration;
	double hard_cut_sensitivity;
	double beat_sensitivity;
	double easter_egg;
	int aspect_correction;
};

/* libprojectM handle. NULL before init and after destroy. Every op
 * guards on this. */
static projectm_handle s_pm;

/* Last value pushed to projectm_set_fps. libprojectM treats repeated
 * identical pushes as a real config change (rebuilds internal state).
 * Dedup here so callers don't have to. -1 means "nothing pushed yet"
 * so the first call always goes through. */
static int s_fps_last_pushed = -1;

/* Last mesh size pushed, same rationale. -1 means "nothing yet". */
static int s_mesh_x_last_pushed = -1;
static int s_mesh_y_last_pushed = -1;

/* Switch-mechanism state, render-thread only. s_pm_drives: 1 when
 * projectM owns procession (hard cut on), set in vz_apply_config. The
 * deferred switch request now lives in playlist state (request_advance),
 * serviced by the render prologue rather than inside vz_render. */
static int s_pm_drives = 0;

/* libprojectM has rare cases where its only signal is a log message.
 * Without this hook installed, that signal is lost to /dev/null and
 * any internal exception goes unexplained. */
#if PM_HAS_LOGGING
static void vz_log_cb(const char *msg, projectm_log_level level, void *ud) {
	(void)ud;
	const char *lvl = "?";
	switch (level) {
		case PROJECTM_LOG_LEVEL_TRACE: lvl = "TRACE"; break;
		case PROJECTM_LOG_LEVEL_DEBUG: lvl = "DEBUG"; break;
		case PROJECTM_LOG_LEVEL_INFO: lvl = "INFO"; break;
		case PROJECTM_LOG_LEVEL_WARN: lvl = "WARN"; break;
		case PROJECTM_LOG_LEVEL_ERROR: lvl = "ERROR"; break;
		case PROJECTM_LOG_LEVEL_FATAL: lvl = "FATAL"; break;
		default: break;
	}
	fprintf(stderr, "[projectm:%s] %s\n", lvl, msg);
}
#endif /* PM_HAS_LOGGING */

static struct pm_config s_cfg;

static void vz_config_defaults(void) {
	snprintf(s_cfg.texture_dir, sizeof(s_cfg.texture_dir),
	         "/usr/share/projectM/presets/textures");
	s_cfg.soft_cut_duration = 3.0;
	s_cfg.hard_cut_enabled = 0;
	s_cfg.hard_cut_duration = 20.0;
	s_cfg.hard_cut_sensitivity = 2.0;
	s_cfg.beat_sensitivity = 1.0;
	s_cfg.easter_egg = 1.0;
	s_cfg.aspect_correction = 1;
}

static int vz_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "texture_dir")) {
		snprintf(s_cfg.texture_dir, sizeof(s_cfg.texture_dir), "%s", val);
		path_strip_trailing_slashes(s_cfg.texture_dir);
		return 1;
	}
	if (!strcmp(k, "soft_cut_duration")) { s_cfg.soft_cut_duration = atof(val); return 1; }
	if (!strcmp(k, "hard_cut_enabled")) { s_cfg.hard_cut_enabled = atoi(val); return 1; }
	if (!strcmp(k, "hard_cut_duration")) { s_cfg.hard_cut_duration = atof(val); return 1; }
	if (!strcmp(k, "hard_cut_sensitivity")) { s_cfg.hard_cut_sensitivity = atof(val); return 1; }
	if (!strcmp(k, "beat_sensitivity")) { s_cfg.beat_sensitivity = atof(val); return 1; }
	if (!strcmp(k, "easter_egg")) { s_cfg.easter_egg = atof(val); return 1; }
	if (!strcmp(k, "aspect_correction")) { s_cfg.aspect_correction = atoi(val); return 1; }
	return 0;
}

static void vz_apply_config(struct visualizer *v) {
	(void)v;
	if (!s_pm) return;

	/* Mesh size from render_params. Seed the dedup gate with the
	 * configured top so later vz_set_mesh_size calls compare against it. */
	size_t mesh_x = (size_t)(render_mesh_x() > 0 ? render_mesh_x() : 48);
	size_t mesh_y = (size_t)(render_mesh_y() > 0 ? render_mesh_y() : 32);
	projectm_set_mesh_size(s_pm, mesh_x, mesh_y);
	s_mesh_x_last_pushed = (int)mesh_x;
	s_mesh_y_last_pushed = (int)mesh_y;

	/* Frame rate from the upstream slice. Seed the dedup gate with
	 * the configured value so later vz_set_fps calls compare against it. */
	int fps = render_fps() > 0 ? render_fps() : 60;
	projectm_set_fps(s_pm, fps);
	s_fps_last_pushed = fps;

	projectm_set_preset_duration(s_pm, playlist_configured_duration());
	s_pm_drives = s_cfg.hard_cut_enabled;
	playlist_set_engine_driven(s_pm_drives);
	projectm_set_soft_cut_duration(s_pm, s_cfg.soft_cut_duration);
	projectm_set_hard_cut_enabled(s_pm, s_cfg.hard_cut_enabled);
	projectm_set_hard_cut_duration(s_pm, s_cfg.hard_cut_duration);
	projectm_set_hard_cut_sensitivity(s_pm, (float)s_cfg.hard_cut_sensitivity);
	projectm_set_beat_sensitivity(s_pm, (float)s_cfg.beat_sensitivity);
	projectm_set_easter_egg(s_pm, (float)s_cfg.easter_egg);
	projectm_set_aspect_correction(s_pm, s_cfg.aspect_correction);

	/* Texture search paths. projectM's own texture_dir first, then every
	 * configured preset root so preset-relative texture references resolve
	 * against the same tree(s). */
	const char *tex_paths[1 + PRESETS_MAX_ROOTS];
	int tp = 0;
	tex_paths[tp++] = s_cfg.texture_dir;
	for (int i = 0; i < presets_dir_count(); i++)
		tex_paths[tp++] = presets_dir_at(i);
	projectm_set_texture_search_paths(s_pm, tex_paths, (size_t)tp);

	DBG("[projectm] soft_cut=%.2fs hard_cut_en=%d pm_fps=%d mesh=%zux%zu",
	    s_cfg.soft_cut_duration,
	    s_cfg.hard_cut_enabled,
	    s_fps_last_pushed > 0 ? s_fps_last_pushed : render_fps(),
	    mesh_x, mesh_y);
}

static void vz_set_window_size(struct visualizer *v, int width, int height) {
	(void)v;
	if (!s_pm) return;
	projectm_set_window_size(s_pm, (size_t)width, (size_t)height);
}

static void vz_set_fps(struct visualizer *v, int rate) {
	(void)v;
	if (!s_pm) return;
	if (rate == s_fps_last_pushed) return;
	projectm_set_fps(s_pm, rate);
	s_fps_last_pushed = rate;
}

static void vz_set_mesh_size(struct visualizer *v, int x, int y) {
	(void)v;
	if (!s_pm) return;
	if (x == s_mesh_x_last_pushed && y == s_mesh_y_last_pushed) return;
	projectm_set_mesh_size(s_pm, (size_t)x, (size_t)y);
	s_mesh_x_last_pushed = x;
	s_mesh_y_last_pushed = y;
}

static double vz_soft_cut_duration(struct visualizer *v) {
	(void)v;
	if (!s_pm) return 0.0;
	return projectm_get_soft_cut_duration(s_pm);
}

/* ProjectM owns procession when hard cut is on. We only REQUEST the
 * advance here. If host owns procession, requests are ignored. */
static void vz_switch_cb(bool is_hard_cut, void *user_data) {
	(void)user_data;
	if (!s_pm_drives) return;
	playlist_request_advance(is_hard_cut); // hard cut => snap
	DBG("[projectm] switch request (hard_cut=%d)", is_hard_cut);
}

static void vz_render(struct visualizer *v) {
	(void)v;
	if (!s_pm) return;
	projectm_opengl_render_frame(s_pm);
}

/* Deposit projectM. Composite a GL texture into the canvas. Save and
 * restore the current blend state so the deposit doesn't leak into
 * surrounding scene composite steps. No-op when PM_HAS_BURN is 0. */
static void vz_deposit(struct visualizer *v, uint32_t tex, int x, int y, int w, int h) {
	(void)v;
#if PM_HAS_BURN
	if (!s_pm) return;

	GLboolean blend_was = glIsEnabled(GL_BLEND);
	GLint b_src_rgb, b_dst_rgb, b_src_a, b_dst_a;
	glGetIntegerv(GL_BLEND_SRC_RGB, &b_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &b_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &b_src_a);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &b_dst_a);

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	projectm_opengl_burn_texture(s_pm, tex, x, y, w, h);

	glBlendFuncSeparate(b_src_rgb, b_dst_rgb, b_src_a, b_dst_a);
	if (!blend_was) glDisable(GL_BLEND);
#else
	(void)tex; (void)x; (void)y; (void)w; (void)h;
#endif
}

static bool vz_load_preset(struct visualizer *v, const char *preset, bool smooth) {
	(void)v;
	if (!s_pm || !preset) return false;
	projectm_load_preset_file(s_pm, preset, smooth);
	return true;
}

static void vz_feed_pcm(struct visualizer *v, const float *data, size_t frames) {
	(void)v;
	if (!s_pm || !data) return;
	projectm_pcm_add_float(s_pm, data, (unsigned int)frames, PROJECTM_STEREO);
}

static bool vz_init(struct visualizer *v, int width, int height) {
	/* Install the log callback FIRST so any errors during projectm_create
	 * (missing OpenGL features, broken shader pipeline, etc.) get
	 * surfaced rather than swallowed. */
#if PM_HAS_LOGGING
	projectm_set_log_callback(vz_log_cb, false, NULL);
	/* Warnings and errors always surface. Full trace only under debug. */
	projectm_set_log_level(g_debug ? PROJECTM_LOG_LEVEL_TRACE
	                               : PROJECTM_LOG_LEVEL_WARN, false);
#endif

	s_pm = projectm_create();
	if (!s_pm) {
		fprintf(stderr, "[projectm] projectm_create failed\n");
		return false;
	}

	projectm_set_window_size(s_pm, (size_t)width, (size_t)height);

	/* Register the switch-request callback (core API). Once set,
	 * projectM defers switching to us instead of acting on its own. */
	projectm_set_preset_switch_requested_event_callback(s_pm, vz_switch_cb, NULL);

	vz_apply_config(v);

#if !PM_HAS_BURN
	fprintf(stderr,
	    "[projectm] burn DISABLED: <projectM-4/render_opengl.h> not found at compile time\n");
#endif

	return true;
}

static void vz_destroy(struct visualizer *v) {
	(void)v;
	if (s_pm) {
		projectm_destroy(s_pm);
		s_pm = NULL;
	}
	s_pm_drives = 0; // Surrender the procession!
	playlist_set_engine_driven(0);
	s_fps_last_pushed = -1;
	s_mesh_x_last_pushed = -1;
	s_mesh_y_last_pushed = -1;
}

static struct visualizer s_projectm_vis = {
	.init = vz_init,
	.destroy = vz_destroy,
	.apply_config = vz_apply_config,
	.set_window_size = vz_set_window_size,
	.set_fps = vz_set_fps,
	.set_mesh_size = vz_set_mesh_size,
	.render = vz_render,
	.load_preset = vz_load_preset,
	.feed_pcm = vz_feed_pcm,
	.soft_cut_duration = vz_soft_cut_duration,
	.deposit = vz_deposit, // projectM advertises deposit
	.sprite = NULL, // reserved
	.priv = NULL,
};

struct visualizer *visualizer_projectm_create(void) {
	return &s_projectm_vis;
}

MODULE_REGISTER(pm,
	.config_prefix = "projectm",
	.config_template =
	"projectm.texture_dir=/usr/share/projectM/presets/textures\n"
	"projectm.soft_cut_duration=0   # seconds. 0 = instant\n"
	"projectm.hard_cut_enabled=1   # bool. reactive transitions\n"
	"projectm.hard_cut_duration=10.0   # seconds. time until hardcut window\n"
	"projectm.hard_cut_sensitivity=1.8   # float. threshold. lower is more sensitive\n"
	"projectm.easter_egg=0.0   # seconds. duration jitter. overrides duration\n"
	"projectm.beat_sensitivity=2.0   # float. 0 is dead\n"
	"projectm.aspect_correction=1   # bool.\n",
	.config_defaults = vz_config_defaults,
	.config_parse = vz_config_parse,
	.file_extensions = ".milk .prjm",
	.create = visualizer_projectm_create);
