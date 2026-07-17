// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file wiring_render.c
 * @brief Prologue and epilogue bodies.
 *
 * Prologue drains audio into the live engine, applies pending actions,
 * and honors the pacer's skip. Epilogue runs after present. */

#define _POSIX_C_SOURCE 200809L

#include "visualizer.h"
#include "audio.h"
#include "module_registry.h"
#include "playlist.h"
#include "info_views.h"
#include "wiring_render.h"
#include "runtime.h"
#include "pending.h"
#include <stdio.h>
#include <stddef.h>
#include <signal.h>

extern int g_debug;
extern _Atomic sig_atomic_t g_running;

#ifndef AUDIO_CHUNK_FRAMES
#define AUDIO_CHUNK_FRAMES 512
#endif
#ifndef AUDIO_CHANNELS
#define AUDIO_CHANNELS 2
#endif

bool wiring_render_prologue(void *user)
{
	struct rt *rt = user;
	if (!g_running) return false;

	const struct frame_pacer *pacer = module_active_pacer();

	/* Harvest GPU-timer results. Fires even on skipped frames. */
	if (pacer && pacer->poll) pacer->poll();

	/* With no pacer the output paces us and we never skip. */
	if (pacer && pacer->should_skip && pacer->should_skip()) return false;

	/* Drain the audio ring into the visualizer each frame. A single chunk
	 * per frame underreads the feed and lags the visuals, so read until
	 * empty. The cap guards against a flooding renderer. */
	float audio_buf[AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS];
	int drain_cap = (AUDIO_RING_FRAMES / AUDIO_CHUNK_FRAMES) + 2;
	for (int i = 0; i < drain_cap; i++) {
		int frames = audio_read(audio_buf, AUDIO_CHUNK_FRAMES);
		if (frames <= 0) break;
		rt->vis->feed_pcm(rt->vis, audio_buf, (size_t)frames);
	}

	apply_pending(rt);

	/* Update the preset name once the soft cut completes */
	if (playlist_transition_in_progress()) {
		if (playlist_transition_elapsed_seconds() >= rt->vis->soft_cut_duration(rt->vis)) {
			info_preset(playlist_transition_text());
			playlist_clear_transition();
		}
	}

	/* Compose the HUD and stage it for overlay */
	info_views_tick();

	if (pacer && pacer->observe) pacer->observe(playlist_transition_in_progress());

	/* Auto-advance timer. Playlist owns the interval, lock, and age. */
	if (playlist_should_auto_advance()) {
		playlist_next();
		playlist_load_current(true);
		playlist_mark_dirty(); // republished next frame
	}

	/* Engine-driven advance. If engine owns procession it
	 * REQUESTS an advance. */
	int eng_adv = playlist_poll_advance();
	if (eng_adv && !playlist_is_locked()) {
		playlist_next();
		playlist_load_current(eng_adv != 2);
		playlist_mark_dirty();
	}

	return true;
}

void wiring_render_epilogue(void *user)
{
	struct rt *rt = user;
	rt->frame_count++;
	const struct frame_pacer *pacer = module_active_pacer();
	if (pacer && pacer->presented) pacer->presented();
	if (g_debug && (rt->frame_count <= 5 || (rt->frame_count % 20000) == 0))
		fprintf(stderr, "[render] frame %d presented\n", rt->frame_count);
}
