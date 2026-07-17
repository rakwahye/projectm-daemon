// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file visualizer.h
 * @brief Visualizer engine vtable.
 *
 * Required ops are always present. Optional ops are NULL when
 * unsupported, so callers must NULL-check and fall back. */

#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct visualizer {
	bool (*init)(struct visualizer *v, int width, int height);
	void (*destroy)(struct visualizer *v);

	/** Re-push the impl's own config slice and the shared render params to
	 * the engine, at bring-up and on reload. Pacing layers, if present,
	 * only modulate on top. */
	void (*apply_config)(struct visualizer *v);
	void (*set_window_size)(struct visualizer *v, int width, int height);
	void (*set_fps)(struct visualizer *v, int rate);
	void (*set_mesh_size)(struct visualizer *v, int x, int y);

	/** Render one frame on the current target and context. */
	void (*render)(struct visualizer *v);

	/** Load an item the visualizer interprets. `preset` is an opaque string.
	 * `smooth` requests a soft transition where supported. The extension
	 * gate picks the engine, so this catches the residual case where the
	 * suffix matched but the content did not.
	 * @returns true if accepted, false if it could not be loaded. */
	bool (*load_preset)(struct visualizer *v, const char *preset, bool smooth);

	/** Feed interleaved stereo float PCM (`frames` sample-frames). */
	void (*feed_pcm)(struct visualizer *v, const float *data, size_t frames);

	/** The active transition (cross-fade) length, in seconds. */
	double (*soft_cut_duration)(struct visualizer *v);

	/** Composite an external GL texture into the visualizer. */
	void (*deposit)(struct visualizer *v, uint32_t tex, int x, int y,
	              int w, int h);

	/** RESERVED. */
	void (*sprite)(struct visualizer *v);

	void *priv;
};

struct visualizer *visualizer_active(void);

struct module_descriptor;

/** Descriptor of the live engine. @returns NULL before the first
 * bring-up. */
const struct module_descriptor *visualizer_active_desc(void);

/** Publish the live engine and its descriptor together. Render-thread
 * only. */
void visualizer_set_active(struct visualizer *vis,
                           const struct module_descriptor *desc);

/** Master render size. */
void visualizer_output_size(int *w, int *h);

/** Stand the engine up. Init at WxH, then publish it as active.
 * @returns false if `vis` is NULL or init fails. */
bool visualizer_bringup(struct visualizer *vis,
                        const struct module_descriptor *desc, int w, int h);

/** Ensure the engine that owns `path` is live, then return it ready to
 * load. Render-thread only. */
struct visualizer *visualizer_ensure_for(const char *path);

#ifdef __cplusplus
}
#endif

#endif
