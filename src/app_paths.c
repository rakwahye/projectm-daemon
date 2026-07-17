// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file app_paths.c
 * @brief Path resolution.
 *
 * Derives every path from the compile-time app id and the XDG base
 * directories, once, on first access. */

#define _POSIX_C_SOURCE 200809L

#include "app_paths.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef APP_ID
#error "APP_ID must be defined at compile time."
#endif

#ifndef REMOTE_ID
#define REMOTE_ID APP_ID
#endif

#define APP_PATH_SZ 256
#define APP_PATH_SZ_BIG 384

static char g_config_dir[APP_PATH_SZ];
static char g_sock_path[APP_PATH_SZ];
static char g_pid_path[APP_PATH_SZ];
static char g_log_path[APP_PATH_SZ_BIG];
static char g_config_file[APP_PATH_SZ_BIG];
static char g_playlists_dir[APP_PATH_SZ_BIG];

static int g_resolved = 0;

static void resolve_once(void)
{
	if (g_resolved) return;
	g_resolved = 1;

	const char *home = getenv("HOME");
	const char *config_home = getenv("XDG_CONFIG_HOME");
	const char *runtime = getenv("XDG_RUNTIME_DIR");

	if (config_home && config_home[0]) {
		snprintf(g_config_dir, sizeof(g_config_dir), "%s/" APP_ID, config_home);
	} else if (home && home[0]) {
		snprintf(g_config_dir, sizeof(g_config_dir), "%s/.config/" APP_ID, home);
	} else {
		snprintf(g_config_dir, sizeof(g_config_dir), "/tmp/" APP_ID);
	}

	if (runtime && runtime[0]) {
		snprintf(g_sock_path, sizeof(g_sock_path), "%s/" APP_ID ".sock", runtime);
		snprintf(g_pid_path, sizeof(g_pid_path), "%s/" APP_ID ".pid", runtime);
	} else {
		snprintf(g_sock_path, sizeof(g_sock_path), "/tmp/" APP_ID ".sock");
		snprintf(g_pid_path, sizeof(g_pid_path), "/tmp/" APP_ID ".pid");
	}

	snprintf(g_log_path, sizeof(g_log_path), "%s/daemon.log", g_config_dir);
	snprintf(g_config_file, sizeof(g_config_file), "%s/config", g_config_dir);
	snprintf(g_playlists_dir, sizeof(g_playlists_dir), "%s/playlists", g_config_dir);
}

const char *app_paths_app_name(void) { return APP_ID; }
const char *app_paths_remote_name(void) { return REMOTE_ID; }
const char *app_paths_config_dir(void) { resolve_once(); return g_config_dir; }
const char *app_paths_sock_path(void) { resolve_once(); return g_sock_path; }
const char *app_paths_pid_path(void) { resolve_once(); return g_pid_path; }
const char *app_paths_log_path(void) { resolve_once(); return g_log_path; }
const char *app_paths_config_file(void) { resolve_once(); return g_config_file; }
const char *app_paths_playlists_dir(void) { resolve_once(); return g_playlists_dir; }
