// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file info_views.h
 * @brief HUD field state.
 *
 * Modules hand their field text here instead of writing the overlay
 * directly. This owns the field state and drives what the HUD shows. */

#ifndef INFO_VIEWS_H
#define INFO_VIEWS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Set the now-playing field. "" or NULL clears. */
void info_now_playing(const char *text);
/** Set the now-playing art path. "" or NULL clears. */
void info_now_playing_art(const char *art_path);
/** Set the preset field. "" or NULL clears. */
void info_preset(const char *text);
/** Set the performance field. "" or NULL clears. */
void info_performance(const char *text);

/** Per-frame HUD driver. Composes the visible field stack, honoring
 * each field's mode and flash timing, and hands the finished string
 * to overlay. Called once per frame from the render prologue. */
void info_views_tick(void);

#ifdef __cplusplus
}
#endif

#endif
