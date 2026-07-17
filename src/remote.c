// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file remote.c
 * @brief Control client.
 *
 * Connects to the daemon's IPC socket, sends one command, and prints
 * the reply. */

#define _POSIX_C_SOURCE 200809L

#include "app_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static const char *find_sock_path(void) {
	const char *env = getenv("VIS_SOCK");
	if (env && env[0]) return env;
	return app_paths_sock_path();
}

static int daemon_is_running(void) {
	const char *sock = find_sock_path();
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return 0;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);

	int ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	close(fd);
	return ok;
}

/* Sent by the daemon when it has taken the socket and wants stdin. */
#define REMOTE_STREAM_ACK "ok stream"

static int remote_send_all(int fd, const void *buf, size_t len) {
	const char *p = buf;
	while (len > 0) {
		ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

static int remote_pump_stdin(int fd) {
	unsigned char buf[65536];
	for (;;) {
		ssize_t r = read(0, buf, sizeof(buf));
		if (r == 0) break;
		if (r < 0) {
			if (errno == EINTR) continue;
			perror("stdin read");
			return 1;
		}
		if (remote_send_all(fd, buf, (size_t)r) < 0) {
			fprintf(stderr, "daemon disconnected: %s\n", strerror(errno));
			return 1;
		}
	}
	return 0;
}

/* Frame: "<verb> argc=N\n" then N NUL-terminated words. NUL separates because
 * it cannot occur inside an argv element, so a word carrying spaces or
 * newlines needs no escaping.
 *
 * The reply's first line decides: REMOTE_STREAM_ACK means pump stdin until
 * EOF, then close. Anything else is a message to print. */
static int remote_send(int argc, char **argv) {
	const char *sock = find_sock_path();
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "socket: %s\n", strerror(errno));
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "daemon not running (cannot connect to %s)\n", sock);
		close(fd);
		return 1;
	}

	char hdr[256];
	int hl = snprintf(hdr, sizeof(hdr), "%s argc=%d\n", argv[0], argc);
	if (hl < 0 || hl >= (int)sizeof(hdr)) {
		fprintf(stderr, "command too long\n");
		close(fd);
		return 1;
	}
	if (remote_send_all(fd, hdr, (size_t)hl) < 0) goto send_failed;
	for (int i = 0; i < argc; i++)
		if (remote_send_all(fd, argv[i], strlen(argv[i]) + 1) < 0) goto send_failed;

	char buf[16384];
	int total = 0;
	int have_line = 0;
	while (total < (int)sizeof(buf) - 1) {
		ssize_t r = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
		if (r <= 0) break;
		total += (int)r;
		buf[total] = '\0';
		if (memchr(buf, '\n', (size_t)total)) { have_line = 1; break; }
	}

	if (have_line && strncmp(buf, REMOTE_STREAM_ACK,
	                         strlen(REMOTE_STREAM_ACK)) == 0) {
		int rc = remote_pump_stdin(fd);
		close(fd);
		return rc;
	}

	if (total > 0) fwrite(buf, 1, (size_t)total, stdout);
	for (;;) { // a reply may run to many lines
		ssize_t r = recv(fd, buf, sizeof(buf), 0);
		if (r <= 0) break;
		fwrite(buf, 1, (size_t)r, stdout);
	}
	fflush(stdout);
	close(fd);
	return 0;

send_failed:
	fprintf(stderr, "send: %s\n", strerror(errno));
	close(fd);
	return 1;
}

/* Prefer the daemon sitting next to this binary. Returning the bare name lets
 * exec fall back to PATH. */
static const char *find_daemon(void) {
	static char path[512];

	ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (len <= 0) return app_paths_app_name();
	path[len] = '\0';

	char *slash = strrchr(path, '/');
	if (!slash) return app_paths_app_name();

	/* Overwrite this binary's own basename with the daemon's. */
	size_t dir_len = (size_t)(slash + 1 - path);
	size_t room = sizeof(path) - dir_len;
	int n = snprintf(slash + 1, room, "%s", app_paths_app_name());
	if (n < 0 || (size_t)n >= room) return app_paths_app_name();

	if (access(path, X_OK) == 0) return path;
	return app_paths_app_name();
}

static int remote_launch(int argc, char **argv) {
	if (daemon_is_running()) {
		fprintf(stderr, "daemon is already running\n");
		return 0;
	}

	/* Collect any -c/--config or -d/--debug args to pass through */
	const char *config_arg = NULL;
	int debug = 0;
	for (int i = 2; i < argc; i++) {
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0)
			&& i + 1 < argc) {
			config_arg = argv[++i];
		} else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
			debug = 1;
		}
	}

	const char *daemon = find_daemon();

	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		return 1;
	}

	if (pid == 0) {
		const char *name = app_paths_app_name();
		if (config_arg && debug)
			execl(daemon, name, "-c", config_arg, "-d", (char *)NULL);
		else if (config_arg)
			execl(daemon, name, "-c", config_arg, (char *)NULL);
		else if (debug)
			execl(daemon, name, "-d", (char *)NULL);
		else
			execl(daemon, name, (char *)NULL);
		fprintf(stderr, "exec %s: %s\n", daemon, strerror(errno));
		_exit(1);
	}

	/* Parent - wait briefly for the socket to appear */
	for (int i = 0; i < 30; i++) {
		struct timespec ts = { 0, 100000000 }; // 100ms
		nanosleep(&ts, NULL);
		if (daemon_is_running()) {
			fprintf(stderr, "daemon started (pid %d)\n", pid);
			return 0;
		}
	}

	fprintf(stderr, "daemon started but socket not responding after 3s\n");
	return 1;
}

/* Offline fallback. The live list comes from the daemon and reflects the
 * modules it was built with. Do not duplicate it here. */
static void remote_usage(void) {
	const char *rn = app_paths_remote_name();
	fprintf(stderr,
		"usage: %s <command> [args...]\n"
		"\n"
		"Run '%s help' for the full command list. It is delivered live by the\n"
		"daemon.\n"
		"\n"
		"lifecycle:\n"
		"  launch [-c conf] [-d]  start the daemon (-d = debug/foreground)\n"
		"  help                   list daemon commands (queries the daemon)\n"
		"  quit                   shut down the daemon\n",
		rn, rn);
}

int main(int argc, char **argv) {
	static char *help_argv[] = { (char *)"help" };

	if (argc < 2) {
		if (daemon_is_running()) return remote_send(1, help_argv);
		remote_usage();
		return 1;
	}

	if (strcmp(argv[1], "help") == 0 ||
		strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		if (daemon_is_running()) return remote_send(1, help_argv);
		remote_usage();
		return 0;
	}

	if (strcmp(argv[1], "launch") == 0)
		return remote_launch(argc, argv); // local. No daemon to ask

	return remote_send(argc - 1, argv + 1);
}
