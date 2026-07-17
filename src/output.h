// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file output.h
 * @brief Frame output vtable.
 *
 * Presents a rendered frame to one output. An output never renders
 * the scene: it either presents the renderer's buffer as-is or
 * re-composites it into its own surface first, per
 * `needs_recomposite`. */

#ifndef OUTPUT_H
#define OUTPUT_H

#include "renderer.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	OUTPUT_WAYLAND, // `wl_surface`
	OUTPUT_HEADLESS = 3, // no output; `timerfd`-paced renderer drive
} output_type_t;

struct output {
	output_type_t type;

	/** false -> present frame's buffer as-is. true -> blit `frame.gl_tex`
	 * and overlay into the output's own surface first. */
	bool needs_recomposite;

	/** Bring the output up. Borrow the renderer for its EGL display,
	 * context, and config for re-compositors, or its `gbm_device`.
	 * @returns true on success. */
	bool (*init)(struct output *c, struct renderer *p);

	void (*destroy)(struct output *c);

	/** Put this frame on the output.
	 * @returns true if presented, false if the frame was dropped. */
	bool (*present)(struct output *c, const struct frame *f);

	/** Each output has its own event source. The shared loop polls the
	 * union of all outputs' fds. */
	int (*get_fd)(struct output *c);

	/** Drain whatever arrived on `get_fd`. Reports the fd readable. */
	void (*dispatch_events)(struct output *c);

	/** Is this output ready for a new frame? The loop renders when any
	 * output is due. */
	bool (*render_due)(struct output *c);

	/** Call this after a successful presentation so the output can clear
	 * its due flag and arm its next event. */
	void (*mark_rendered)(struct output *c);

	/** Output dimensions, for the blit's NDC and viewport. May differ from
	 * the renderer's master dimensions. */
	int (*get_width)(struct output *c);
	int (*get_height)(struct output *c);

	/** Output refresh in Hz, or 0 if unknown. OPTIONAL. May be `NULL`. */
	int (*get_refresh_hz)(struct output *c);

	void *priv;
};

/** Surface role for surface-based re-compositing outputs. Owned by the
 * output layer. The Wayland output maps these to its shell role
 * internally. */
typedef enum {
	OUTPUT_ROLE_AUTO, // wallpaper if layer-shell present, else windowed
	OUTPUT_ROLE_WALLPAPER, // wlr-layer-shell background surface
	OUTPUT_ROLE_WINDOWED, // xdg-toplevel window
} output_role_t;

struct output *output_wayland_create(output_role_t role);

/** Headless output. Presents nothing. A `timerfd` pacer that drives the
 * shared loop so the renderer keeps rendering at a steady cadence.
 * `present()` discards the frame. */
struct output *headless_create(void);

#ifdef __cplusplus
}
#endif

#endif
