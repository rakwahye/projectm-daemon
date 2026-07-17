// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wiring_render.h
 * @brief Per-frame render glue.
 *
 * The prologue and epilogue the loop runs around each frame. */

#ifndef WIRING_RENDER_H
#define WIRING_RENDER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Returns false to skip rendering this frame. */
bool wiring_render_prologue(void *user);

/** Runs after the rendered frame is presented. */
void wiring_render_epilogue(void *user);

#ifdef __cplusplus
}
#endif

#endif
