// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file loop.h
 * @brief Shared render loop.
 *
 * Polls all output fds and, when an output is due, runs prologue,
 * render, present to all outputs, then epilogue. */

#ifndef LOOP_H
#define LOOP_H

#include "renderer.h"
#include <signal.h>
#include "output.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-frame prologue. Return false to skip this frame. */
typedef bool (*loop_prologue_fn)(void *user);

/** Optionally called once after a frame is presented to all outputs.
 * May be `NULL`. */
typedef void (*loop_epilogue_fn)(void *user);

struct loop {
	struct renderer *renderer;
	struct output **outputs;
	int n_outputs;

	loop_prologue_fn prologue;
	loop_epilogue_fn epilogue;
	void *user; // passed to prologue/epilogue
};

/** Run the loop until the callers `running` becomes 0. Blocks. Returns
 * 0 on clean exit, non-zero if a fatal error shut it down. */
int loop_run(struct loop *lp, _Atomic sig_atomic_t *running);

#ifdef __cplusplus
}
#endif

#endif
