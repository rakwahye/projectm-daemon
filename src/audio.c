// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file audio.c
 * @brief PCM feed socket and lock-free ring.
 *
 * The renderer owns head, the output owns tail. Free-running 64-bit
 * counters, slot = idx % CAP. The renderer never blocks and never
 * touches tail. On overrun the output drops the oldest frames itself, so
 * each counter has exactly one writer and the lock-free claim holds. */

#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdint.h>
#include <poll.h>
#include <sys/eventfd.h>

#define AUDIO_RING_CAP AUDIO_RING_FRAMES

static float g_ring[AUDIO_RING_CAP * AUDIO_CHANNELS];
static atomic_uint_least64_t g_ring_head = 0; // renderer-only
static atomic_uint_least64_t g_ring_tail = 0; // output-only

/* Mirrored from the ambient g_debug at audio_init. */
extern int g_debug;
static int g_audio_debug = 0;

static struct audio_config s_cfg;

static void audio_config_defaults(void) {
	snprintf(s_cfg.addr, sizeof(s_cfg.addr), "127.0.0.1");
	s_cfg.port = 9100;
}

static int audio_config_parse(const char *subkey, const char *val) {
	if (!strcmp(subkey, "port")) {
		s_cfg.port = atoi(val);
		return 1;
	}
	if (!strcmp(subkey, "addr")) {
		snprintf(s_cfg.addr, sizeof(s_cfg.addr), "%s", val);
		return 1;
	}
	return 0;
}

static void ring_write(const float *data, int frames) {
	uint64_t head = atomic_load_explicit(&g_ring_head, memory_order_relaxed);
	for (int i = 0; i < frames; i++) {
		int off = (int)(head % AUDIO_RING_CAP) * AUDIO_CHANNELS;
		for (int c = 0; c < AUDIO_CHANNELS; c++)
			g_ring[off + c] = data[i * AUDIO_CHANNELS + c];
		head++;
	}
	atomic_store_explicit(&g_ring_head, head, memory_order_release);
}

static int ring_read(float *out, int max_frames) {
	uint64_t head = atomic_load_explicit(&g_ring_head, memory_order_acquire);
	uint64_t tail = atomic_load_explicit(&g_ring_tail, memory_order_relaxed);

	/* Drop oldest on overrun here, so tail stays single-writer. */
	if (head - tail > AUDIO_RING_CAP) tail = head - AUDIO_RING_CAP;

	int n = 0;
	while (n < max_frames && tail != head) {
		int off = (int)(tail % AUDIO_RING_CAP) * AUDIO_CHANNELS;
		for (int c = 0; c < AUDIO_CHANNELS; c++)
			out[n * AUDIO_CHANNELS + c] = g_ring[off + c];
		tail++;
		n++;
	}
	atomic_store_explicit(&g_ring_tail, tail, memory_order_release);
	return n;
}

/* Consumer-safe occupancy gauge, clamped to capacity. */
unsigned audio_ring_fill(void) {
	uint64_t head = atomic_load_explicit(&g_ring_head, memory_order_acquire);
	uint64_t tail = atomic_load_explicit(&g_ring_tail, memory_order_acquire);
	uint64_t fill = head - tail;
	return (unsigned)(fill > AUDIO_RING_CAP ? AUDIO_RING_CAP : fill);
}

static int g_listen_fd = -1;
static int g_client_fd = -1;

/* eventfd. teardown wakes the blocked worker, fds closed after join */
static int g_wake_fd = -1;

static pthread_t g_thread;
static _Atomic int s_run = 0; // worker run flag

/* Returns 1 on full read, 0 on peer EOF, -1 on error, -2 if woken for
 * teardown (the wake eventfd became readable). Polls before recv so
 * blocked read is interruptible without closing the fd under it. */
static int read_full(int fd, int wake, void *buf, size_t want) {
	char *p = (char *)buf;
	size_t got = 0;
	while (got < want) {
		struct pollfd pf[2] = { { fd, POLLIN, 0 }, { wake, POLLIN, 0 } };
		if (poll(pf, 2, -1) < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (pf[1].revents) return -2; // teardown wake
		if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
		ssize_t n = recv(fd, p + got, want - got, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return 0;
		got += (size_t)n;
	}
	return 1;
}

static void *reader_thread(void *arg) {
	(void)arg;

	const size_t chunk_bytes = AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS * sizeof(float);
	float *buf = malloc(chunk_bytes);
	if (!buf) {
		fprintf(stderr, "[audio] malloc failed\n");
		return NULL;
	}
	unsigned peak_fill = 0, writes_since_report = 0, last_reported_ms = (unsigned)-1;

	while (atomic_load(&s_run)) {
		if (g_client_fd < 0) {
			struct pollfd pf[2] = {
				{ g_listen_fd, POLLIN, 0 },
				{ g_wake_fd, POLLIN, 0 }
			};
			if (poll(pf, 2, -1) < 0) {
				if (errno == EINTR) continue;
				break;
			}
			if (pf[1].revents) break; // teardown wake
			if (!(pf[0].revents & POLLIN)) continue;
			struct sockaddr_in cli;
			socklen_t clen = sizeof(cli);
			g_client_fd = accept(g_listen_fd, (struct sockaddr *)&cli, &clen);
			if (g_client_fd < 0) {
				if (errno == EINTR && !atomic_load(&s_run)) break;
				continue;
			}
			/* Realtime PCM. Disable Nagle so the kernel forwards each small
			 * chunk immediately instead of coalescing it into bursts. */
			{
				int one = 1;
				setsockopt(g_client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			}
			char ipbuf[INET_ADDRSTRLEN] = {0};
			inet_ntop(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));
			if (g_audio_debug) {
				fprintf(stderr, "[audio] producer connected: %s:%u\n",
				        ipbuf, (unsigned)ntohs(cli.sin_port));
			}
		}

		int rc = read_full(g_client_fd, g_wake_fd, buf, chunk_bytes);
		if (rc == -2) break; // teardown wake
		if (rc <= 0) {
			if (rc == 0) {
				if (g_audio_debug) {
					fprintf(stderr, "[audio] producer disconnected\n");
				}
			}
			else {
				fprintf(stderr, "[audio] recv error: %s\n", strerror(errno));
			}
			close(g_client_fd);
			g_client_fd = -1;
			continue;
		}

		ring_write(buf, AUDIO_CHUNK_FRAMES);

		if (g_audio_debug) {
			/* Sample peak occupancy over a ~1s window, but only
			 * emit when the rounded value changes. */
			unsigned fill = audio_ring_fill();
			if (fill > peak_fill) peak_fill = fill;
			if (++writes_since_report >= AUDIO_SAMPLE_RATE / AUDIO_CHUNK_FRAMES) {
				unsigned ms = peak_fill * 1000u / AUDIO_SAMPLE_RATE;
				if (ms != last_reported_ms) {
					fprintf(stderr, "[audio] ring peak %ums of %ums\n",
						ms, (unsigned)(AUDIO_RING_CAP * 1000u / AUDIO_SAMPLE_RATE));
					last_reported_ms = ms;
				}
				peak_fill = 0;
				writes_since_report = 0;
			}
		}
	}

	free(buf);
	return NULL;
}

int audio_init(struct rt *rt) {
	(void)rt;
	g_audio_debug = g_debug;
	const char *address = s_cfg.addr;
	int port = s_cfg.port;
	if (port <= 0 || port > 65535) return 0;
	/* Empty address falls back to loopback, not all-interfaces. */
	if (!address[0]) address = "127.0.0.1";

	signal(SIGPIPE, SIG_IGN);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "[audio] socket: %s\n", strerror(errno));
		return 0;
	}

	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
		fprintf(stderr, "[audio] bad address: %s\n", address);
		close(fd);
		return 0;
	}

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[audio] bind %s:%d: %s\n", address,
		        port, strerror(errno));
		close(fd);
		return 0;
	}

	if (listen(fd, 1) < 0) {
		fprintf(stderr, "[audio] listen: %s\n", strerror(errno));
		close(fd);
		return 0;
	}

	g_listen_fd = fd;

	g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (g_wake_fd < 0) {
		fprintf(stderr, "[audio] eventfd: %s\n", strerror(errno));
		close(fd);
		g_listen_fd = -1;
		return 0;
	}

	atomic_store(&s_run, 1);

	if (pthread_create(&g_thread, NULL, reader_thread, NULL) != 0) {
		fprintf(stderr, "[audio] pthread_create: %s\n", strerror(errno));
		close(fd);
		g_listen_fd = -1;
		close(g_wake_fd);
		g_wake_fd = -1;
		atomic_store(&s_run, 0);
		return 0;
	}

	if (g_audio_debug) {
		fprintf(stderr,
		        "[audio] listening on %s:%d (float32 %dHz %dch)\n",
		        address, port, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);
	}
	return 1;
}

int audio_read(float *out, int max_frames) {
	return ring_read(out, max_frames);
}

void audio_shutdown(void) {
	atomic_store(&s_run, 0);

	/* Wake the worker via the eventfd rather than closing the fd under it.
	 * the fds are closed only AFTER the join, so nothing races the close
	 * against accept()/recv(). */
	if (g_wake_fd >= 0) {
		uint64_t one = 1;
		ssize_t w = write(g_wake_fd, &one, sizeof(one));
		(void)w;
	}

	pthread_join(g_thread, NULL);

	if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
	if (g_client_fd >= 0) { close(g_client_fd); g_client_fd = -1; }
	if (g_wake_fd >= 0) { close(g_wake_fd); g_wake_fd = -1; }
}

MODULE_REGISTER(audio,
	.init = audio_init,
	.shutdown = audio_shutdown,
	.config_prefix = "audio",
	.config_template =
		"audio.port=9100   # port\n"
		"audio.addr=127.0.0.1\n",
	.config_defaults = audio_config_defaults,
	.config_parse = audio_config_parse);
