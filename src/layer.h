// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file layer.h
 * @brief Scene Z-order: in-scene back vs above-scene front. */

#ifndef LAYER_H
#define LAYER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Z-order layer. Modes without layer_shell clamp to BACK. */
typedef enum {
	SCENE_LAYER_BACK = 0, // in-scene GL composite
	SCENE_LAYER_FRONT = 1, // above-scene Wayland LAYER_OVERLAY surface
} scene_layer_t;

#ifdef __cplusplus
}
#endif

#endif
