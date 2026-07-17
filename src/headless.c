// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file headless.c
 * @brief Headless output.
 *
 * Owns nothing: the renderer draws every frame off-screen regardless,
 * so this needs no GBM, EGL, or DRM. Purely a pacer. A timerfd drives
 * the shared poll loop like any other output fd, no busy-spin. */

#define _POSIX_C_SOURCE 200809L

#include "output.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/timerfd.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

struct headless_priv {
	struct renderer *prod; // borrowed - for master dimensions only
	int tfd; // the loop's periodic wake `timerfd`
	int due; // set by `dispatch_events`, cleared by `mark_rendered`
	long frame_ns; // cadence
};

static bool headless_init(struct output *c, struct renderer *p)
{
	struct headless_priv *hp = c->priv;
	hp->prod = p;

	const struct frame_pacer *pacer = module_active_pacer();
	int ceiling = (pacer && pacer->fps_ceiling) ? pacer->fps_ceiling() : 0;
	int fps = ceiling > 0 ? ceiling : 60;
	hp->frame_ns = 1000000000L / fps;

	hp->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (hp->tfd < 0) {
		fprintf(stderr, "[headless] timerfd_create: %s\n", strerror(errno));
		return false;
	}

	struct itimerspec its = {
		.it_interval = { .tv_sec = hp->frame_ns / 1000000000L,
		                 .tv_nsec = hp->frame_ns % 1000000000L },
		.it_value = { .tv_sec = 0, .tv_nsec = 1 }, // fire ~immediately
	};
	if (timerfd_settime(hp->tfd, 0, &its, NULL) < 0) {
		fprintf(stderr, "[headless] timerfd_settime: %s\n", strerror(errno));
		close(hp->tfd);
		hp->tfd = -1;
		return false;
	}

	hp->due = 1; // render the first frame promptly (loop primes once)
	DBG("[headless] up: %d fps (%ld ns/frame), output discarded", fps, hp->frame_ns);
	return true;
}

static void headless_destroy(struct output *c)
{
	if (!c) return;
	struct headless_priv *hp = c->priv;
	if (hp) {
		if (hp->tfd >= 0) close(hp->tfd);
		free(hp);
	}
	free(c);
}

/* No output. The renderer's BO ring recycles `frame.bo` on the next render.
 * A re-composite output would sample `frame.gl_tex` here, but we want the
 * pixels nowhere. Counts as "presented" so the loop clears the `due` flag. */
static bool headless_present(struct output *c, const struct frame *f)
{
	(void)c; (void)f;
	return true;
}

static int headless_get_fd(struct output *c)
{
	return ((struct headless_priv *)c->priv)->tfd;
}

static void headless_dispatch_events(struct output *c)
{
	struct headless_priv *hp = c->priv;
	uint64_t expirations = 0;
	/* Drain all pending expirations. One or more means "time to render". */
	while (read(hp->tfd, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations))
		; // `TFD_NONBLOCK`: read until `EAGAIN`
	hp->due = 1;
}

static bool headless_render_due(struct output *c)
{
	return ((struct headless_priv *)c->priv)->due;
}

static void headless_mark_rendered(struct output *c)
{
	((struct headless_priv *)c->priv)->due = 0;
}

static int headless_get_width(struct output *c)
{
	struct headless_priv *hp = c->priv;
	return hp->prod ? hp->prod->width : 0;
}

static int headless_get_height(struct output *c)
{
	struct headless_priv *hp = c->priv;
	return hp->prod ? hp->prod->height : 0;
}

struct output *headless_create(void)
{
	struct output *c = calloc(1, sizeof(*c));
	if (!c) return NULL;
	struct headless_priv *hp = calloc(1, sizeof(*hp));
	if (!hp) { free(c); return NULL; }
	hp->tfd = -1;

	c->type = OUTPUT_HEADLESS;
	/* Headless is NEITHER a raw scanout nor a re-compositor. It presents
	 * nothing. We report `true` so the wiring's master-size logic treats it
	 * like a re-compositor - i.e. it shares no renderer BO and is exempt
	 * from the raw-scanout mode-equality constraint (which would otherwise
	 * misclassify a headless display as a buffer-sharing scanout and could
	 * abort a headless+scanout combo). `present()` is a no-op anyway. */
	c->needs_recomposite = true;
	c->priv = hp;
	c->init = headless_init;
	c->destroy = headless_destroy;
	c->present = headless_present;
	c->get_fd = headless_get_fd;
	c->dispatch_events = headless_dispatch_events;
	c->render_due = headless_render_due;
	c->mark_rendered = headless_mark_rendered;
	c->get_width = headless_get_width;
	c->get_height = headless_get_height;
	return c;
}
