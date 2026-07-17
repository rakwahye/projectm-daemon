// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file visualizer_null.c
 * @brief Fallback engine. Always registered.
 *
 * Shown when no engine is loaded. Fills the frame with a flat color
 * whose brightness tracks audio level, so a silent black frame reads as
 * failure while a pulsing one proves the pipeline is live.
 *
 * Covers the whole vtable and leaves `deposit` NULL to keep composite
 * fallback exercised. */

#include "visualizer.h"
#include "module_registry.h"
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <GLES3/gl3.h>

static int s_w, s_h;
static float s_energy; // smoothed recent PCM levels

static bool null_init(struct visualizer *v, int width, int height) {
	(void)v;
	s_w = width; s_h = height;
	s_energy = 0.0f;
	return true;
}

static void null_destroy(struct visualizer *v) {
	(void)v;
	s_w = s_h = 0;
	s_energy = 0.0f;
}

static void null_apply_config(struct visualizer *v) { (void)v; }
static void null_set_window_size(struct visualizer *v, int w, int h) {
	(void)v; s_w = w; s_h = h;
}
static void null_set_fps(struct visualizer *v, int rate) {
	(void)v; (void)rate;
}
static void null_set_mesh_size(struct visualizer *v, int x, int y) {
	(void)v; (void)x; (void)y;
}

static void null_render(struct visualizer *v) {
	(void)v;
	s_energy *= 0.92f; // decay. feed_pcm re-attacks
	float b = s_energy > 1.0f ? 1.0f : s_energy;
	glClearColor(b * 0.10f, b * 0.40f, b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static bool null_load_preset(struct visualizer *v, const char *item, bool smooth) {
	(void)v; (void)smooth;
	return item != NULL;
}

static void null_feed_pcm(struct visualizer *v, const float *data, size_t frames) {
	(void)v;
	if (!data || frames == 0) return;
	size_t samples = frames * 2; // stereo
	float acc = 0.0f;
	for (size_t i = 0; i < samples; i++) acc += fabsf(data[i]);
	float mag = acc / (float)samples;
	if (mag > s_energy) s_energy = mag;
}

static double null_soft_cut_duration(struct visualizer *v) { (void)v; return 0.0; }

static struct visualizer s_null_vis = {
	.init = null_init,
	.destroy = null_destroy,
	.apply_config = null_apply_config,
	.set_window_size = null_set_window_size,
	.set_fps = null_set_fps,
	.set_mesh_size = null_set_mesh_size,
	.render = null_render,
	.load_preset = null_load_preset,
	.feed_pcm = null_feed_pcm,
	.soft_cut_duration = null_soft_cut_duration,
	.deposit = NULL,
	.sprite = NULL,
	.priv = NULL,
};

struct visualizer *visualizer_null_create(void) {
	return &s_null_vis;
}

MODULE_REGISTER(null, .create = visualizer_null_create);
