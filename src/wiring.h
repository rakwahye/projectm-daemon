// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wiring.h
 * @brief Daemon assembly and top-level run. */

#ifndef WIRING_H
#define WIRING_H

#include "config.h"
#include "output.h"
#include <signal.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rt;

/** Top-level run. Renderer up, backend bringup, derive master size from the
 * outputs, finalize the pipeline, run, tear down. Renderer and outputs are
 * constructed from whatever the backend bring-up resolves. `rt` is the owned
 * runtime object, threaded onto the loop's `void *user`. */
int run_daemon(_Atomic sig_atomic_t *running, struct rt *rt);

#ifdef __cplusplus
}
#endif

#endif
