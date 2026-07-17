// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file audio.h
 * @brief Audio input.
 *
 * A TCP listener that accepts raw float32 stereo PCM into a ring the
 * render thread drains each frame. No library dependencies. */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_CHUNK_FRAMES 512

/** Ring buffer holds ~1 second of audio. Render thread drains what's
 * available each frame, so this is plenty of slack. */
#define AUDIO_RING_FRAMES (AUDIO_SAMPLE_RATE)

struct audio_config {
	char addr[64]; // Bind address for the PCM listener
	int port;
};

struct rt;

/** Start the audio listener. Bind the address/port. Spawns thread.
 * @returns 1 = success. */
int audio_init(struct rt *rt);

/** Non-blocking read from the ring buffer.  Returns frames read (0 if empty). */
int audio_read(float *out, int max_frames);

/** Ring occupancy in frames. A latency gauge. Multiply by
 * 1000 / AUDIO_SAMPLE_RATE for the milliseconds of audio held in
 * the ring. */
unsigned audio_ring_fill(void);

void audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
