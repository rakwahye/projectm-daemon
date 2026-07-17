// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file render_params.h
 * @brief Shared render parameters.
 *
 * Mesh size and frame rate that engines read through the accessors,
 * rather than each owning its own copy. */

#ifndef RENDER_PARAMS_H
#define RENDER_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif

struct render_params_config {
	int mesh_x, mesh_y, fps;
};

int render_mesh_x(void);
int render_mesh_y(void);
int render_fps(void);

#ifdef __cplusplus
}
#endif

#endif
