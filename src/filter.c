// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file filter.c
 * @brief Live filter state.
 *
 * Holds the render-time filter color, updated from a request mailbox
 * so IPC changes apply without tearing. */

#define _GNU_SOURCE
#include "filter.h"
#include "gl_quad.h"
#include "color.h"
#include "module_registry.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* Live values used at render time. Updated from a pending-request
mailbox atomically under g_lock so cross-thread updates don't tear. */
static float g_r, g_g, g_b, g_a;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_req_r, g_req_g, g_req_b, g_req_a;
static int g_req_pending;

static int g_inited;

static struct filter_config s_cfg;

static void filter_config_defaults(void) {
	s_cfg.r = s_cfg.g = s_cfg.b = s_cfg.a = 0.0f; // off by default
}

static int filter_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "tint")) {
		color_parse_hex(val, &s_cfg.r, &s_cfg.g, &s_cfg.b, &s_cfg.a);
		filter_config_apply();
		return 1;
	}
	return 0;
}

void filter_config_apply(void) {
	filter_set(s_cfg.r, s_cfg.g, s_cfg.b, s_cfg.a);
}

static void filter_render_init(void) { filter_init(); }
static void filter_render_at(int w, int h) { (void)w; (void)h; filter_render(); }

/* Fill from the pending request when one is queued, else from the live
 * values. */
static void filter_current(float *r, float *g, float *b, float *a) {
	pthread_mutex_lock(&g_lock);
	int p = g_req_pending;
	*r = p ? g_req_r : g_r;
	*g = p ? g_req_g : g_g;
	*b = p ? g_req_b : g_b;
	*a = p ? g_req_a : g_a;
	pthread_mutex_unlock(&g_lock);
}

/* 6 hex digits set color, 8 set color and alpha, anything else
 * parses as a fraction that sets alpha. */
static int filter_ipc_command(struct ipc_command_ctx *c) {
	char *reply = c->reply;
	int reply_len = c->reply_len;

	float r, g, b, a;
	filter_current(&r, &g, &b, &a);

	const char *hex = c->args;
	if (*hex == '#') hex++;
	float nr, ng, nb, na;

	if (color_parse_hex(c->args, &nr, &ng, &nb, &na)) {
		r = nr; g = ng; b = nb;
		if (strlen(hex) == 8) a = na;
	} else if (sscanf(c->args, "%f", &a) != 1) {
		snprintf(reply, reply_len,
		         "err usage: tint <hex RGBA> | tint <fraction>\n");
		module_emit_reply("tint", reply);
		return 0;
	}

	filter_set(r, g, b, a);

	char out[9];
	color_to_hex(r, g, b, a, out);
	snprintf(reply, reply_len, "ok tint %s\n", out);
	module_emit_reply("tint", reply);
	return 0;
}

int filter_init(void) {
	if (g_inited) return 1;
	g_r = g_g = g_b = g_a = 0.0f;
	g_req_pending = 0;
	g_inited = 1;
	filter_config_apply();
	return 1;
}

void filter_set(float r, float g, float b, float a) {
	/* Clamp to sane range here so render-side stays simple. */
	if (a < 0.0f) a = 0.0f;
	if (a > 1.0f) a = 1.0f;
	if (r < 0.0f) r = 0.0f;
	if (r > 1.0f) r = 1.0f;
	if (g < 0.0f) g = 0.0f;
	if (g > 1.0f) g = 1.0f;
	if (b < 0.0f) b = 0.0f;
	if (b > 1.0f) b = 1.0f;

	pthread_mutex_lock(&g_lock);
	g_req_r = r; g_req_g = g; g_req_b = b; g_req_a = a;
	g_req_pending = 1;
	pthread_mutex_unlock(&g_lock);
}

void filter_render(void) {
	if (!g_inited) return;

	/* Drain the mailbox once per frame. */
	pthread_mutex_lock(&g_lock);
	if (g_req_pending) {
		g_r = g_req_r; g_g = g_req_g; g_b = g_req_b; g_a = g_req_a;
		g_req_pending = 0;
	}
	pthread_mutex_unlock(&g_lock);

	if (g_a <= 0.0f) return; // off

	gl_quad_tint(g_r, g_g, g_b, g_a);
}

void filter_shutdown(void) {
	g_inited = 0;
}

MODULE_REGISTER(filter,
	.config_prefix = "filter",
	.config_template =
		"filter.tint=00000000   # hex RGBA\n",
	.config_defaults = filter_config_defaults,
	.config_parse = filter_config_parse,
	.config_apply = filter_config_apply,
	.render_phase = RENDER_PHASE_EFFECT,
	.render_init = filter_render_init,
	.render = filter_render_at,
	.render_destroy = filter_shutdown,
	.ipc_verb = "tint",
	.ipc_command = filter_ipc_command,
	.ipc_help =
		"\ntint:\n"
		"  tint <hex RGB[A]>  set color and opacity\n"
		"  tint <fraction>    set opacity. 1.0 = opaque\n");
