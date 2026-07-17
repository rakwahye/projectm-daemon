// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file nowplaying.h
 * @brief Now-playing watcher.
 *
 * A background thread polls playerctl and, on track change, fetches
 * album art and pushes text and art to the overlay. A no-op when
 * playerctl or curl are absent. */

#ifndef NOWPLAYING_H
#define NOWPLAYING_H

#ifdef __cplusplus
extern "C" {
#endif

struct rt;
int nowplaying_start(struct rt *rt);
void nowplaying_stop(void);

#ifdef __cplusplus
}
#endif

#endif
