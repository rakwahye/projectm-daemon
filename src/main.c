// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file main.c
 * @brief Entry point.
 *
 * Parses CLI and config, starts IPC, seeds the runtime object, and
 * hands off to the daemon run. Owns `rt` and initializes its mailbox
 * lock before any thread starts. */

#define _GNU_SOURCE

#include "config.h"
#include "ipc.h"
#include "visualizer.h"
#include "playlist.h"
#include "wiring.h"
#include "cli.h"
#include "scene.h"
#include "app_paths.h"
#include "runtime.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>

#ifndef VISUALIZER_FACTORY
#error "VISUALIZER_FACTORY undefined"
#endif

_Atomic sig_atomic_t g_running = 1;
int g_debug = 0;

/* Shared runtime. Passed to `run_daemon` and then to each module's
 * init */
static struct rt g_rt;

#define DBG(fmt, ...) \
do { if (g_debug) \
	fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
} while(0)

struct visualizer *VISUALIZER_FACTORY(void);

/* Active visualizer, set once the factory runs. */
struct visualizer *visualizer_active(void) {
	return g_rt.vis;
}

const struct module_descriptor *visualizer_active_desc(void) {
	return g_rt.vis_desc;
}

void visualizer_set_active(struct visualizer *vis,
                           const struct module_descriptor *desc) {
	g_rt.vis = vis;
	g_rt.vis_desc = desc;
}

/* Master render size for mid-stream engine bring-up */
void visualizer_output_size(int *w, int *h) {
	if (w) *w = atomic_load(&g_rt.output_w);
	if (h) *h = atomic_load(&g_rt.output_h);
}

static void sig_handler(int sig) {
	(void)sig;
	g_running = 0;
}

static int ipc_load_preset(const char *path) {
	if (access(path, R_OK) != 0) return 0;
	rt_pending_post_path(&g_rt, PENDING_LOAD, path);
	return 1;
}

static void ipc_quit(void) {
	g_running = 0;
}

/* Output size. Written on configure/resize by wiring and read by the IPC
 * thread to resolve coord-language fractions to pixel rects. Stored as
 * _Atomic, so the cross-thread read needs no lock. Reads 0 until the first
 * output configures. IPC errors the open in that window. */
void main_set_output_size(int w, int h) {
	atomic_store(&g_rt.output_w, w);
	atomic_store(&g_rt.output_h, h);
}

static void ipc_reload(void) {
	rt_pending_post(&g_rt, PENDING_RELOAD);
}

/* Bring up playlist selection and the visualizer engine at master size. */
bool setup_visualizer(int width, int height, int vrefresh_hz,
                      const void *pace_ctx) {
	/* Selects the current preset before the engine comes up, so we can stand
	 * up the engine that preset actually needs. */
	playlist_bringup();

	/* Resolve the initial engine from the first preset and bring it up
	 * cold. An empty playlist or an unresolvable path falls back to the
	 * build's primary engine so the scene always has a live renderer. This
	 * is the only cold bring-up. Every later change of format goes through
	 * `visualizer_ensure_for`. */
	const struct module_descriptor *want = NULL;
	if (playlist_count() > 0)
		want = module_engine_for(playlist_current_path());
	if (!want)
		want = module_engine_find(VISUALIZER_FACTORY);
	if (!want) {
		fprintf(stderr, "[main] no engine registered for initial bring-up\n");
		return false;
	}

	if (!visualizer_bringup(want->create(), want, width, height))
		return false;

	const struct frame_pacer *pacer = module_active_pacer();
	if (pacer && pacer->init) pacer->init(vrefresh_hz, pace_ctx);

	playlist_load_initial();

	DBG("[visualizer] initialized %dx%d", width, height);
	return true;
}

static void write_pid(const char *path) {
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	}
}

static void ensure_parent_dir(const char *filepath) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", filepath);
	char *slash = strrchr(tmp, '/');
	if (slash) {
		*slash = '\0';
		struct stat st;
		if (stat(tmp, &st) != 0)
			mkdir(tmp, 0755);
	}
}

static void daemonize(const char *log_path) {
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		exit(1);
	}
	if (pid > 0) exit(0);

	if (setsid() < 0) exit(1);

	pid = fork();
	if (pid < 0) exit(1);
	if (pid > 0) exit(0);

	/* Redirect stderr to log file */
	ensure_parent_dir(log_path);
	int logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (logfd >= 0) {
		dup2(logfd, STDERR_FILENO);
		if (logfd != STDERR_FILENO) close(logfd);
	}

	close(STDIN_FILENO);
	close(STDOUT_FILENO);

	if (chdir("/") < 0) {}

	fprintf(stderr, "\n--- %s started (pid %d) ---\n", app_paths_app_name(), getpid());
}

static const char *s_cli_config_path = NULL;

static int main_cli_config(const char *optarg) {
	s_cli_config_path = optarg;
	return 0;
}

static int main_cli_debug(const char *optarg) {
	(void)optarg;
	g_debug = 1;
	return 0;
}

int main(int argc, char **argv) {
	/* Sticky overrides route through the config apply path. */
	cli_set_override_sink(config_apply_kv);

	/* Core options, registered before the registry walk. -d drives debug,
	 * -c selects the config file read below. -c can't be a sticky key
	 * since it picks the file the keys come from. */
	static const struct cli_option core_opts[] = {{
		.long_name = "config", .short_name = 'c',
		.has_arg = CLI_REQUIRED_ARG, .help = "config file path",
		.handler = main_cli_config
	},{
		.long_name = "debug", .short_name = 'd',
		.has_arg = CLI_NO_ARG, .help = "enable debug logging",
		.handler = main_cli_debug
	},};
	cli_register("core", core_opts, 2);

	/* `cli_parse` walks the module registry so each module/backend adds its
	 * own option group. */
	if (!cli_parse(argc, argv)) return 1;

	/* Load config. CLI -c path wins. */
	if (s_cli_config_path)
		config_load_from(s_cli_config_path);
	else
		config_load();

	/* Apply sticky CLI overrides over the file before anything reads
	 * config. Announced pre-daemonize so they reach the log. Sticky
	 * across reloads. */
	cli_apply_overrides(true);

	/* Early timer seed from playlist's duration slice, before
	 * setup, so bring-up info IPC reports the configured value. */
	playlist_set_auto_advance(playlist_configured_duration());

	/* Pending-block mutex lives in rt. Initialize it before any
	 * thread that touches it starts.  */
	pthread_mutex_init(&g_rt.pending.lock, NULL);

	/* Daemonize unless debug mode */
	if (!g_debug)
		daemonize(app_paths_log_path());
	else
		fprintf(stderr, "[debug] running in foreground\n");

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	write_pid(app_paths_pid_path());

	/* Module init pass. Aborts on the first module that reports failure. */
	const struct module_descriptor *failed = module_registry_init_all(&g_rt);
	if (failed) {
		fprintf(stderr, "[main] %s init failed\n", failed->name);
		/* Shut down the modules that did init, reverse order. */
		module_registry_unwind(failed);
		unlink(app_paths_pid_path());
		return 1;
	}

	/* IPC carries main-owned callbacks, so it starts here. */
	struct ipc_callbacks ipc_cb = {
		.load_preset = ipc_load_preset,
		.reload = ipc_reload,
		.quit = ipc_quit,
	};

	if (!ipc_start(&ipc_cb, &g_rt)) {
		fprintf(stderr, "IPC init failed\n");
		/* Every module initialized before IPC. Shut them all down. */
		module_registry_unwind(NULL);
		unlink(app_paths_pid_path());
		return 1;
	}

	int rc = run_daemon(&g_running, &g_rt);

	/* Cleanup only the non-GL subsystems. */
	const struct frame_pacer *pacer = module_active_pacer();
	if (pacer && pacer->summary) pacer->summary();

	fprintf(stderr, "[main] shutting down\n");

	/* Reverse-order teardown of every module that initialized */
	module_registry_unwind(NULL);

	playlist_shutdown();
	unlink(app_paths_pid_path());
	return rc;
}
