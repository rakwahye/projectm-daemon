// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file loop.c
 * @brief Poll, render, present.
 *
 * The frame engine, holding no policy of its own. A skipped prologue,
 * dropped render, or busy output is non-fatal. Only a fatal error ends
 * the loop. */

#define _POSIX_C_SOURCE 200809L

#include "loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

/* Render one frame and present it to every due output. A skipped
 * prologue, dropped render, or busy output is non-fatal. */
static void render_and_present(struct loop *lp)
{
	if (lp->prologue && !lp->prologue(lp->user))
		return;

	struct frame f;
	if (!renderer_render(lp->renderer, &f))
		return;

	for (int i = 0; i < lp->n_outputs; i++) {
		struct output *c = lp->outputs[i];
		if (!c->render_due(c)) continue;
		if (c->present(c, &f))
			c->mark_rendered(c);
		/* A dropped present leaves the output due to retry next render. */
	}

	if (lp->epilogue) lp->epilogue(lp->user);
}

int loop_run(struct loop *lp, _Atomic sig_atomic_t *running)
{
	if (!lp->renderer || lp->n_outputs < 1) {
		fprintf(stderr, "[loop] need a renderer and >=1 output\n");
		return 1;
	}

	struct pollfd *pfds = calloc(lp->n_outputs, sizeof(*pfds));
	if (!pfds) return 1;

	DBG("[loop] entering render loop, %d output(s)", lp->n_outputs);

	/* Render one frame before blocking in poll, so the first
	 * frame-callback or flip is armed. */
	render_and_present(lp);

	while (*running) {
		for (int i = 0; i < lp->n_outputs; i++) {
			pfds[i].fd = lp->outputs[i]->get_fd(lp->outputs[i]);
			pfds[i].events = POLLIN;
			pfds[i].revents = 0;
		}

		/* An already-due output means render now, else wait on an fd. */
		int any_due = 0;
		for (int i = 0; i < lp->n_outputs; i++)
			if (lp->outputs[i]->render_due(lp->outputs[i])) { any_due = 1; break; }

		int timeout = any_due ? 0 : -1;
		int rc = poll(pfds, lp->n_outputs, timeout);
		if (rc < 0) {
			if (errno == EINTR) continue; // signal, recheck running
			fprintf(stderr, "[loop] poll: %s\n", strerror(errno));
			break;
		}

		/* Drain outputs whose fd fired. */
		for (int i = 0; i < lp->n_outputs; i++) {
			if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP))
				lp->outputs[i]->dispatch_events(lp->outputs[i]);
		}

		/* Render once if any output is due. */
		any_due = 0;
		for (int i = 0; i < lp->n_outputs; i++)
			if (lp->outputs[i]->render_due(lp->outputs[i])) { any_due = 1; break; }

		if (any_due)
			render_and_present(lp);
	}

	DBG("[loop] exiting render loop");
	free(pfds);
	return 0;
}
