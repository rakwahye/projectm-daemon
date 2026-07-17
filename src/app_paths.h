// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file app_paths.h
 * @brief Resolved application paths.
 *
 * Accessors return cached, statically-stored strings, resolved lazily
 * on first call, never NULL. Init is assumed single-threaded: resolved
 * at startup before any worker thread. */

#ifndef APP_PATHS_H
#define APP_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

const char *app_paths_app_name(void);
const char *app_paths_remote_name(void);
const char *app_paths_config_dir(void);
const char *app_paths_sock_path(void);
const char *app_paths_pid_path(void);
const char *app_paths_log_path(void);
const char *app_paths_config_file(void);
const char *app_paths_playlists_dir(void);

#ifdef __cplusplus
}
#endif

#endif
