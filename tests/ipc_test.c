#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#include "ipc.h"
#include "module_registry.h"

#include "check.h"

/* The one peer the module links against directly, stubbed so this stays a
 * pure ipc link check. */
struct rt;
int playlist_ipc_command(struct rt *mbox, const char *line, char *reply, int reply_len) {
	(void)mbox; (void)line; (void)reply; (void)reply_len; return 0;
}

static const struct module_descriptor *g_ipc = NULL;

static void find_ipc(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "ipc") == 0) g_ipc = d;
}

/* Registered as a real module, so the frame decoder is exercised through the
 * same path a shipping verb takes. It records whatever it was handed. */
#define PROBE_MAX 16
static int   g_probe_argc;
static char  g_probe_argv[PROBE_MAX][256];
static char  g_probe_args[512];
static char  g_probe_body[256];
static int   g_probe_body_len;
static int   g_probe_hits;

static int probe_ipc(struct ipc_command_ctx *c) {
	g_probe_hits++;
	g_probe_argc = c->argc < PROBE_MAX ? c->argc : PROBE_MAX;
	for (int i = 0; i < g_probe_argc; i++)
		snprintf(g_probe_argv[i], sizeof(g_probe_argv[i]), "%s", c->argv[i]);
	snprintf(g_probe_args, sizeof(g_probe_args), "%s", c->args ? c->args : "");
	g_probe_body_len = c->body_len;
	int n = c->body_len < (int)sizeof(g_probe_body) - 1
			? c->body_len : (int)sizeof(g_probe_body) - 1;
	if (n > 0) memcpy(g_probe_body, c->body, (size_t)n);
	g_probe_body[n > 0 ? n : 0] = '\0';
	snprintf(c->reply, c->reply_len, "ok probe\n");
	return 0;
}

MODULE_REGISTER(probe,
	.ipc_verb    = "probe",
	.ipc_command = probe_ipc);

static char g_sock[100];

/* One write per frame. The peer replies and closes as soon as the frame is
 * complete, so a trailer posted after that would race the close. */
static int send_frame(int argc, const char *const *argv,
					  const void *trailer, size_t trailer_len) {
	char frame[4096];
	size_t off = 0;
	int hl = snprintf(frame, sizeof(frame), "%s argc=%d\n", argv[0], argc);
	if (hl < 0) return -1;
	off = (size_t)hl;
	for (int i = 0; i < argc; i++) {
		size_t wl = strlen(argv[i]) + 1;
		if (off + wl > sizeof(frame)) return -1;
		memcpy(frame + off, argv[i], wl);
		off += wl;
	}
	if (trailer_len) {
		if (off + trailer_len > sizeof(frame)) return -1;
		memcpy(frame + off, trailer, trailer_len);
		off += trailer_len;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_sock);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }

	if (send(fd, frame, off, MSG_NOSIGNAL) < 0) { close(fd); return -1; }

	char reply[256];
	ssize_t r = recv(fd, reply, sizeof(reply) - 1, 0);
	(void)r;
	close(fd);
	return 0;
}

static int send_raw(const char *s) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_sock);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
	if (send(fd, s, strlen(s), MSG_NOSIGNAL) < 0) { close(fd); return -1; }
	char reply[256];
	ssize_t r = recv(fd, reply, sizeof(reply) - 1, 0);
	(void)r;
	close(fd);
	return 0;
}

static void probe_reset(void) {
	g_probe_argc = 0; g_probe_args[0] = '\0';
	g_probe_body[0] = '\0'; g_probe_body_len = 0;
}

static int frame_tests(void) {
	snprintf(g_sock, sizeof(g_sock), "/tmp/ipc-frame-test.%d.sock", (int)getpid());
	unlink(g_sock);

	g_ipc->config_defaults();
	CHECK(g_ipc->config_parse("sock_path", g_sock) == 1, "the socket path is configurable, so the test can isolate itself");

	struct ipc_callbacks cb = { NULL, NULL, NULL };
	CHECK(ipc_start(&cb, NULL) != 0, "the listener starts on the configured path");

	probe_reset();
	const char *sp[] = { "probe", "-f", "DejaVu Sans" };
	CHECK(send_frame(3, sp, NULL, 0) == 0, "a framed command reaches the verb");
	CHECK(g_probe_argc == 3, "a word containing a space stays one word");
	CHECK(strcmp(g_probe_argv[1], "-f") == 0, "the flag before a spaced word survives");
	CHECK(strcmp(g_probe_argv[2], "DejaVu Sans") == 0, "the spaced word arrives whole");

	probe_reset();
	const char *nl[] = { "probe", "-t", "a\nb", "-t", "c" };
	CHECK(send_frame(5, nl, NULL, 0) == 0, "a command carrying a newline inside a word is delivered");
	CHECK(g_probe_argc == 5, "an embedded newline does not split a word into two");
	CHECK(strcmp(g_probe_argv[2], "a\nb") == 0, "the word with the newline is not truncated at it");
	CHECK(strcmp(g_probe_argv[4], "c") == 0, "the word after the newline is not swallowed");

	probe_reset();
	const char *ar[] = { "probe", "60" };
	CHECK(send_frame(2, ar, NULL, 0) == 0, "a two-word command is delivered");
	CHECK(strcmp(g_probe_args, "60") == 0, "args still reads as the old flat command line");

	probe_reset();
	const char *st[] = { "probe", "-sw", "16" };
	CHECK(send_frame(3, st, "\x01\x02\x03\x04", 4) == 0, "a command with a binary trailer is delivered");
	CHECK(g_probe_argc == 3, "trailer bytes are not counted as words");
	CHECK(g_probe_body_len == 4, "bytes past the last word arrive as the body");
	CHECK(memcmp(g_probe_body, "\x01\x02\x03\x04", 4) == 0, "the body arrives byte for byte");

	probe_reset();
	CHECK(send_raw("probe hello\n") == 0, "a header with no argc is still accepted");
	CHECK(g_probe_argc == 2, "a header with no argc splits on spaces");
	CHECK(strcmp(g_probe_argv[0], "probe") == 0, "the verb is the first split word");
	CHECK(strcmp(g_probe_argv[1], "hello") == 0, "the remaining split word follows it");
	CHECK(strcmp(g_probe_args, "hello") == 0, "args is the tail of the split line");

	CHECK(g_probe_hits == 5, "every command reached the verb exactly once, with none dropped or doubled");

	ipc_stop();
	unlink(g_sock);
	return 0;
}

int main(void) {
	module_registry_visit(find_ipc, NULL);
	CHECK(g_ipc != NULL, "ipc registers a descriptor under prefix ipc");
	CHECK(g_ipc->config_defaults != NULL, "ipc supplies a defaults hook");
	CHECK(g_ipc->config_parse != NULL, "ipc supplies a parse hook");
	CHECK(g_ipc->shutdown != NULL, "ipc registers a shutdown hook, so the listener closes");

	g_ipc->config_defaults();
	CHECK(g_ipc->config_parse("sock_path", "/tmp/x.sock") == 1, "parse claims the sock_path key");
	CHECK(g_ipc->config_parse("bogus", "x") == 0, "parse rejects an unknown key");
	CHECK(g_ipc->config_parse("port", "9100") == 0, "parse rejects port, which another module owns");

	if (frame_tests() != 0) return 1;

	CHECK_PASS("test-ipc");
}
