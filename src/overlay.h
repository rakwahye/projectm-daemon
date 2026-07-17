// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file overlay.h
 * @brief Album-art and text overlay surface. */

#ifndef OVERLAY_H
#define OVERLAY_H

#include "coord.h"
#include "layer.h"
#include <wayland-client.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where album art sits relative to the view text. */
enum overlay_art_placement {
	OVERLAY_ART_BEHIND = 0, // text overlaid on top of art
	OVERLAY_ART_LEFT,
	OVERLAY_ART_RIGHT,
	OVERLAY_ART_ABOVE,
	OVERLAY_ART_BELOW,
};

struct overlay_style {
	char font_family[128];
	double font_size;
	float color_r, color_g, color_b, color_a; // config/CLI form: 8-hex RRGGBBAA
	float stroke_r, stroke_g, stroke_b, stroke_a;
	double stroke_width;

	coord_length_t pos_x;
	coord_length_t pos_y;
	coord_length_t size_w; // unspecified = full
	coord_length_t size_h; // unspecified = full
	coord_anchor_t anchor; // corner of the rect that pos refers to

	int art_placement; // enum overlay_art_placement
	double art_size_frac; // art bounding box, fraction of min(surf_w,surf_h)
	float art_alpha;

	int layer; // scene_layer_t (SCENE_LAYER_BACK/FRONT)

	int transient_duration; // ms. command flashes via overlay_push_transient
	int info_duration; // ms. how long `info` peek stays up
};

/** Fill style with sane defaults. */
void overlay_style_defaults(struct overlay_style *s);

/** Overlay's config slice. */
struct overlay_config {
	struct overlay_style style;
	int burn; // 1 = Composite the burn into the canvas
	int burn_ms;
	int fade_in_ms; // Transient fade-in
	int fade_out_ms; // Transient fade-out
	int flash; // For command-feedback messages
};

/** Initialize the overlay. Call after Wayland globals are bound. */
int overlay_init(struct wl_compositor *comp,
                 struct wl_shm *shm,
                 void *layer_shell,
                 struct wl_output *output,
                 int output_width, int output_height);

/** Re-apply overlay's config slice to live state. Stages the style for
 * the next tick and refreshes the burn and fade knobs. */
void overlay_config_apply(void);

/** Force the configured layer to BACK (in-scene GL). Called from the wirings
 * when no layer-shell is available, before `overlay_init`. `mode_name` is for the
 * log line only. */
void overlay_clamp_layer_to_back(const char *mode_name);

/** Accessor for peers that need an overlay config value. */
int overlay_burn_enabled(void);

/** Replace style live. Takes effect on next tick. */
void overlay_update_style(const struct overlay_style *style);

/** Update output dimensions for resolving fractional coord-language
 * lengths (e.g. overlay_x=0.5 -> display_w/2). Call after the daemon's
 * render surface has been resized. The new dims take effect on the next
 * surface create. */
void overlay_set_output_size(int w, int h);

/** Tear down. */
void overlay_destroy(void);

/** The steady HUD. The caller composes the full multi-line string and
 * hands it here. Overlay renders it and fades it in and out as it
 * appears and clears. NULL or "" clears the view. show_art selects whether
 * the cached art renders with this view. Applied on next tick. */
void overlay_set_view(const char *text, int show_art);
void overlay_set_art(const char *art_path);

/** Transients.
 * Push a short-lived message on top of the slots. duration_ms=0 means
 * use style.transient_duration. Newest transient replaces currently
 * showing. */
void overlay_push_transient(const char *text, int duration_ms);

/** Show a peek of system state for style.info_duration. text is the
 * composed info content. show_art controls whether album art renders
 * behind the text during the peek. Implemented as a special transient
 * mode. */
void overlay_show_info(const char *text, int show_art);

/** Overlay.
 * Self-contained view payload. Text + optional image + its own overrides.
 * Runs through the transient pipeline (fade, burn, queue).
 * All fields except `text` are optional. Use OVERLAY_SPEC_DEFAULT (-1)
 * for "use style default". */
#define OVERLAY_SPEC_DEFAULT (-1)

struct overlay_spec {
	char text[2048]; // multi-line via \n. "" = no text
	char image_path[1024]; // jpeg or png path on disk. "" = no image

	int duration_ms; // including fades. OVERLAY_SPEC_DEFAULT = style.info_duration
	int fade_in_ms; // OVERLAY_SPEC_DEFAULT = use overlay_fade_in_ms
	int fade_out_ms; // OVERLAY_SPEC_DEFAULT = use overlay_fade_out_ms

	int show_art; // 0 = no art, 1 = show (image_path if set, else now-playing cache)
	float art_alpha; // 0.0..1.0. OVERLAY_SPEC_DEFAULT = style
	float art_size_frac; // 0.0..1.0. OVERLAY_SPEC_DEFAULT = style
	int art_placement; // enum overlay_art_placement

	int burn; // 0 = off, 1 = on. OVERLAY_SPEC_DEFAULT = style/config. queue-aware suppression still applies

	char text_color[16]; // 8-hex RRGGBBAA. "" = style
	char stroke_color[16]; // 8-hex RRGGBBAA. "" = style
	double text_size; // font size px. <0 = style
	double stroke_width; // <0 = style
	char font[128]; // "" = style

	coord_length_t pos_x; // present=0 = fall back to style
	coord_length_t pos_y;
	coord_length_t size_w;
	coord_length_t size_h;
	int anchor; // coord_anchor_t cast. -1 = use style

	int layer; // SCENE_LAYER_BACK/FRONT. -1 = style. mid-stream switch: teardown + rebuild
};

/** Initialize an overlay with all sentinel defaults. Call this first, then
 * fill in only what you want to override. */
static inline void overlay_spec_init(struct overlay_spec *d) {
	d->text[0] = '\0';
	d->image_path[0] = '\0';
	d->duration_ms = OVERLAY_SPEC_DEFAULT;
	d->fade_in_ms = OVERLAY_SPEC_DEFAULT;
	d->fade_out_ms = OVERLAY_SPEC_DEFAULT;
	d->show_art = 0;
	d->art_alpha = (float)OVERLAY_SPEC_DEFAULT;
	d->art_size_frac = (float)OVERLAY_SPEC_DEFAULT;
	d->art_placement = OVERLAY_SPEC_DEFAULT;
	d->burn = OVERLAY_SPEC_DEFAULT;
	d->text_color[0] = '\0';
	d->stroke_color[0] = '\0';
	d->text_size = OVERLAY_SPEC_DEFAULT;
	d->stroke_width = OVERLAY_SPEC_DEFAULT;
	d->font[0] = '\0';
	d->pos_x = coord_length_unspecified();
	d->pos_y = coord_length_unspecified();
	d->size_w = coord_length_unspecified();
	d->size_h = coord_length_unspecified();
	d->anchor = OVERLAY_SPEC_DEFAULT;
	d->layer = OVERLAY_SPEC_DEFAULT;
}

/** Parse an overlay's flag grammar into `d`. `argv[0]` is the verb.
 * Repeated `-t` lines are joined with '\n' in order. A word arrives
 * byte-exact, so a text line may itself contain spaces or newlines.
 * Anything not named keeps its `overlay_spec_init` sentinel.
 * @returns false on a bad or value-less flag, with reason in `err`. */
bool overlay_spec_parse(int argc, char **argv, struct overlay_spec *d,
                           char *err, size_t errlen);

/** Queue an overlay. Newest overlay replaces any currently-showing
 * transient (or in-flight overlay). Burn from the superseded overlay
 * gets cancelled per the queue-aware rule.
 *
 * If `d->image_path` is set, the file is decoded inside tick (synchronously,
 * jpeg/png). Decode failure: the overlay still presents, just without the
 * image. The path is read once at apply time and not held. Caller can
 * free or reuse the buffer immediately. */
void overlay_show(const struct overlay_spec *d);

/** BACK layer only. Upload pending texture data if needed and draw the quad
 * into the currently-bound framebuffer. `output_w` and `output_h` describe
 * the destination framebuffer, not the overlay rectangle. */
void overlay_render_present(int output_w, int output_h);

/** Poll for a pending burn. `out_tex` is a pre-scaled scratch texture,
 * `out_x` and `out_y` and `out_w` and `out_h` the pixel rect, `out_alpha`
 * the sine-bell curve value. One-shot: a pending burn is cleared whether
 * or not the caller goes on to run it.
 * @returns 1 if a burn should fire. 0 when no burn pending. */
int overlay_poll_burn(uint32_t *out_tex, int *out_x, int *out_y,
                                 int *out_w, int *out_h, float *out_alpha);

/** Set the wall-clock burn duration in milliseconds. Internally the output
 * uses wall-clock elapsed time since snapshot to compute burn phase. Any
 * in-flight burn keeps its current remaining duration. Values < 1 are
 * clamped to 1. */
void overlay_set_burn_ms(int ms);

/** Set visible fade-in and fade-out durations (ms). In-scene mode only.
 * 0 disables that side. Affects future transients only. */
void overlay_set_fade_durations(int fade_in_ms, int fade_out_ms);

/** Call every frame from the main thread. Handles Wayland I/O, art decoding,
 * expiry timers, and surface lifecycle. */
void overlay_tick(void);

#ifdef __cplusplus
}
#endif

#endif
