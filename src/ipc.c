// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file ipc.c
 * @brief Command socket and dispatch.
 *
 * A unix-socket thread reads commands and runs each verb's handler.
 * Handlers that touch engine state queue a pending action rather than
 * act on the socket thread. */

#define _POSIX_C_SOURCE 200809L

#include "ipc.h"
#include "runtime.h"
#include "app_paths.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

static int g_sock_fd = -1;

/* Eventfd. Teardown wakes the blocked worker, fd closed after join */
static int g_wake_fd = -1;

static pthread_t g_thread;
static _Atomic int s_run = 0;
static struct ipc_callbacks g_cb;

/* Runtime. handed over at ipc_start read only for display dims */
static struct rt *g_ipc_rt;

static char g_sock_path[256];

static struct ipc_config s_cfg;

static void ipc_config_defaults(void) {
	snprintf(s_cfg.sock_path, sizeof(s_cfg.sock_path), "%s",
	         app_paths_sock_path());
}

static int ipc_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "sock_path")) {
		snprintf(s_cfg.sock_path, sizeof(s_cfg.sock_path), "%s", val);
		return 1;
	}
	return 0;
}

static void send_reply(int fd, const char *msg) {
	size_t len = strlen(msg);
	while (len > 0) {
		ssize_t n = send(fd, msg, len, MSG_NOSIGNAL);
		if (n <= 0) break;
		msg += n;
		len -= (size_t)n;
	}
}

struct ipc_cmd_ctx {
	const char *line; char *reply; int reply_len; int handled;
	int fd; const char *body; int body_len; int output_w;
	int output_h; int owned; int argc; char **argv;
};

static void ipc_try_command(const struct module_descriptor *d,
                            void *ud) {
	struct ipc_cmd_ctx *c = ud;
	if (c->handled || !d->ipc_command) return;
	const char *args;
	if (d->ipc_verb) {
		size_t vl = strlen(d->ipc_verb);
		if (strncmp(c->line, d->ipc_verb, vl) != 0) return;
		if (c->line[vl] != '\0' && c->line[vl] != ' ') return;
		args = c->line + vl;
		while (*args == ' ') args++;
	} else {
		/* Line-handler. Sees entire line, may decline */
		args = c->line;
	}
	struct ipc_command_ctx cc = {
		.fd = c->fd, .args = args,
		.argc = c->argc, .argv = c->argv,
		.body = c->body, .body_len = c->body_len,
		.output_w = c->output_w, .output_h = c->output_h,
		.reply = c->reply, .reply_len = c->reply_len,
	};
	int rc = d->ipc_command(&cc);
	if (!d->ipc_verb && rc == IPC_CMD_DECLINED) return; // not its verb
	c->owned = rc;
	c->handled = 1;
}

struct ipc_help_ctx { char *buf; int off; int len; };

static void ipc_collect_help(const struct module_descriptor *d, void *ud) {
	struct ipc_help_ctx *c = ud;
	if (d->ipc_help && c->off < c->len)
		c->off += snprintf(c->buf + c->off, c->len - c->off, "%s",
						   d->ipc_help);
}

/* Wire Format:
 *     <verb> argc=N\n
 *     <arg0>\0<arg1>\0...\0
 *     [residual]
 *
 * NUL separated because it is the one byte that cannot occur inside an argv
 * element, so a word may carry spaces or newlines with nothing to escape.
 *
 * Residual is whatever follows the Nth NUL: the head of a stream that shared a
 * packet with its open command. Handed on as `body`.
 *
 * A header with no `argc=` is split on spaces. */
#define IPC_MAX_ARGS 128

struct ipc_frame {
	int argc;
	char *argv[IPC_MAX_ARGS];
	char line[4096]; // argv space-joined, as the verb sees it
	const char *body;
	int body_len;
};

static int ipc_count_nuls(const char *raw, size_t from, size_t to) {
	int n = 0;
	for (size_t i = from; i < to; i++) if (raw[i] == '\0') n++;
	return n;
}

/* Never NUL-terminate inside the counted region. The count is over
 * bytes the peer actually sent. False on hangup, overrun, or malformed
 * header. */
static bool ipc_recv_frame(int fd, char *raw, size_t cap, struct ipc_frame *f) {
	size_t n = 0;
	char *nl = NULL;

	for (;;) {
		nl = memchr(raw, '\n', n);
		if (nl) break;
		if (n >= cap) return false;
		ssize_t r = recv(fd, raw + n, cap - n, 0);
		if (r <= 0) return false;
		n += (size_t)r;
	}
	*nl = '\0';
	size_t body_off = (size_t)(nl - raw) + 1;

	char *hdr = raw;
	size_t hl = strlen(hdr);
	while (hl > 0 && hdr[hl - 1] == '\r') hdr[--hl] = '\0';

	int want = -1;
	char *a = strstr(hdr, " argc=");
	if (a) {
		want = atoi(a + 6);
		*a = '\0'; // hdr is now the bare verb
		if (want < 0 || want > IPC_MAX_ARGS) return false;
	}

	f->argc = 0;

	if (want >= 0) {
		while (ipc_count_nuls(raw, body_off, n) < want) {
			if (n >= cap) return false;
			ssize_t r = recv(fd, raw + n, cap - n, 0);
			if (r <= 0) return false;
			n += (size_t)r;
		}
		char *p = raw + body_off;
		for (int i = 0; i < want; i++) {
			f->argv[f->argc++] = p;
			p += strlen(p) + 1;
		}
		f->body = p;
		f->body_len = (int)(n - (size_t)(p - raw));
	} else {
		char *cur = hdr;
		while (*cur && f->argc < IPC_MAX_ARGS) {
			while (*cur == ' ') cur++;
			if (!*cur) break;
			f->argv[f->argc++] = cur;
			while (*cur && *cur != ' ') cur++;
			if (*cur) *cur++ = '\0';
		}
		f->body = raw + body_off;
		f->body_len = (int)(n - body_off);
	}

	int off = 0;
	for (int i = 0; i < f->argc; i++) {
		int w = snprintf(f->line + off, sizeof(f->line) - (size_t)off,
		                 "%s%s", i ? " " : "", f->argv[i]);
		if (w < 0 || (size_t)(off + w) >= sizeof(f->line)) break;
		off += w;
	}
	f->line[sizeof(f->line) - 1] = '\0';
	return true;
}

/* Returns 0 normally, caller closes fd. 1 if fd ownership transferred and the
 * caller MUST NOT close. */
static int handle_client(int fd) {
	static char raw[65536]; // IPC thread only
	struct ipc_frame f;

	if (!ipc_recv_frame(fd, raw, sizeof(raw), &f)) return 0;
	if (f.argc == 0) return 0;

	char *line = f.line;
	const char *body = f.body;
	size_t body_len = (size_t)f.body_len;

	char reply[8192];

	if (strncmp(line, "preset ", 7) == 0) {
		const char *path = line + 7;
		while (*path == ' ') path++;
		if (g_cb.load_preset && g_cb.load_preset(path)) {
			const char *bn = strrchr(path, '/');
			bn = bn ? bn + 1 : path;
			snprintf(reply, sizeof(reply), "ok preset: %s\n", bn);
		} else {
			snprintf(reply, sizeof(reply), "err failed to load preset\n");
		}
		module_emit_reply("ipc", reply);

	} else if (strcmp(line, "reload") == 0) {
		if (g_cb.reload) g_cb.reload();
		snprintf(reply, sizeof(reply), "ok config reloaded\n");
		module_emit_reply("ipc", reply);

	} else if (strcmp(line, "quit") == 0) {
		send_reply(fd, "ok bye\n");
		if (g_cb.quit) g_cb.quit();
		return 0;

	} else if (strcmp(line, "help") == 0) {
		static char hb[16384];
		struct ipc_help_ctx hc = { hb, 0, sizeof(hb) };
		hc.off += snprintf(hb + hc.off, sizeof(hb) - hc.off,
			"commands (the remote forwards these to the daemon):\n"
			"\n"
			"preset:\n"
			"  preset <path>         load a specific preset file\n"
			"\n"
			"lifecycle:\n"
			"  reload               reload the config file\n"
			"  quit                 shut down the daemon\n");
		module_registry_visit(ipc_collect_help, &hc);
		send_reply(fd, hb);
		return 0;

	} else {
		struct ipc_cmd_ctx ctx = {
			.line = line, .reply = reply, .reply_len = sizeof(reply),
			.handled = 0, .fd = fd, .body = body,
			.body_len = (int)body_len,
			.argc = f.argc, .argv = f.argv,
			.output_w = g_ipc_rt ? atomic_load(&g_ipc_rt->output_w) : 0,
			.output_h = g_ipc_rt ? atomic_load(&g_ipc_rt->output_h) : 0,
			.owned = 0,
		};
		module_registry_visit(ipc_try_command, &ctx);
		if (ctx.handled) {
			if (ctx.owned) return 1; // Module took fd. Do not close
		} else {
			snprintf(reply, sizeof(reply), "err unknown: %s\n", line);
			module_emit_reply("ipc", reply);
		}
	}

	send_reply(fd, reply);
	return 0; // Normal path. Caller closes fd
}

static void *ipc_thread(void *arg) {
	(void)arg;

	while (atomic_load(&s_run)) {
		struct pollfd pf[2] = {
			{ g_sock_fd, POLLIN, 0 },
			{ g_wake_fd, POLLIN, 0 }
		};

		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (pf[1].revents) break; // Teardown wake
		if (!(pf[0].revents & POLLIN)) continue;
		int client = accept(g_sock_fd, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR && !atomic_load(&s_run)) break;
			continue;
		}

		int owned = handle_client(client);
		if (!owned) close(client);
	}

	return NULL;
}

int ipc_start(const struct ipc_callbacks *cb, struct rt *rt) {
	if (!cb) return 0;
	g_ipc_rt = rt;
	const char *sock_path = s_cfg.sock_path;

	g_cb = *cb;
	snprintf(g_sock_path, sizeof(g_sock_path), "%s", sock_path);

	unlink(g_sock_path);

	g_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_sock_fd < 0) {
		fprintf(stderr, "[ipc] socket: %s\n", strerror(errno));
		return 0;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(g_sock_path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "[ipc] socket path too long (%zu >= %zu): %s\n",
		        strlen(g_sock_path), sizeof(addr.sun_path), g_sock_path);
		close(g_sock_fd);
		g_sock_fd = -1;
		return 0;
	}
	memcpy(addr.sun_path, g_sock_path, strlen(g_sock_path) + 1);

	/* Create socket node 0600, via umask around bind. */
	mode_t old_umask = umask(0177); // ~0177 = 0600
	int bind_rc = bind(g_sock_fd, (struct sockaddr *)&addr, sizeof(addr));
	int bind_errno = errno;
	umask(old_umask); // restore, umask is process-wide
	if (bind_rc < 0) {
		errno = bind_errno;
		fprintf(stderr, "[ipc] bind %s: %s\n", g_sock_path, strerror(errno));
		close(g_sock_fd);
		g_sock_fd = -1;
		return 0;
	}

	if (listen(g_sock_fd, 4) < 0) {
		fprintf(stderr, "[ipc] listen: %s\n", strerror(errno));
		close(g_sock_fd);
		g_sock_fd = -1;
		return 0;
	}

	g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (g_wake_fd < 0) {
		fprintf(stderr, "[ipc] eventfd: %s\n", strerror(errno));
		close(g_sock_fd);
		g_sock_fd = -1;
		return 0;
	}

	atomic_store(&s_run, 1);

	if (pthread_create(&g_thread, NULL, ipc_thread, NULL) != 0) {
		fprintf(stderr, "[ipc] pthread_create: %s\n", strerror(errno));
		close(g_sock_fd);
		g_sock_fd = -1;
		close(g_wake_fd);
		g_wake_fd = -1;
		atomic_store(&s_run, 0);
		return 0;
	}

	fprintf(stderr, "[ipc] listening on %s\n", g_sock_path);
	return 1;
}

void ipc_stop(void) {
	/* No-op if we never started or start failed. The registry's shutdown
	 * walk runs this for every module, and joining an unstarted thread is
	 * undefined. `s_run` is set only after successful start and cleared on
	 * every failure path, so it is the reliable "did we start" gate. */
	if (!atomic_load(&s_run)) return;
	atomic_store(&s_run, 0);

	/* Wake the worker via eventfd rather than closing the fd beneath
	 * it. The socket is closed only AFTER the join, so nothing races
	 * the close against accept(). */
	if (g_wake_fd >= 0) {
		uint64_t one = 1;
		ssize_t w = write(g_wake_fd, &one, sizeof(one));
		(void)w;
	}

	pthread_join(g_thread, NULL);

	if (g_sock_fd >= 0) { close(g_sock_fd); g_sock_fd = -1; }
	if (g_wake_fd >= 0) { close(g_wake_fd); g_wake_fd = -1; }
	unlink(g_sock_path);
}

MODULE_REGISTER(ipc,
	.shutdown = ipc_stop,
	.config_prefix = "ipc",
	.config_defaults = ipc_config_defaults,
	.config_parse = ipc_config_parse);
