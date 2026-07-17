// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wayland.h
 * @brief Wayland compositor globals for front-layer modules. */

#ifndef WAYLAND_H
#define WAYLAND_H

struct wl_compositor;
struct wl_shm;
struct wl_output;
struct zwlr_layer_shell_v1;

#ifdef __cplusplus
extern "C" {
#endif

/** Compositor object accessors. Return NULL before init() runs or after
 * destroy(). */
struct wl_compositor *wayland_get_compositor(void);
struct wl_shm *wayland_get_shm(void);
struct wl_output *wayland_get_output(void);
struct zwlr_layer_shell_v1 *wayland_get_layer_shell(void);

#ifdef __cplusplus
}
#endif

#endif
