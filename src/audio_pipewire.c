// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file audio_pipewire.c
 * @brief PipeWire capture feeder.
 *
 * Captures from the graph and pushes the result into the daemon's
 * PCM socket.  */

#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

extern int g_debug;
#define DBG(fmt, ...) do { if (g_debug) fprintf(stderr, fmt "\n", ##__VA_ARGS__); } while(0)

#define SILENCE_RELEASE_MS 2000
#define SILENCE_RELEASE_FRAMES \
	((uint64_t)SILENCE_RELEASE_MS * AUDIO_SAMPLE_RATE / 1000u)

struct pipewire_config {
	int enabled;
	int capture_sink; // 1 = default monitor, 0 = default source
	char target[128]; // node.name to pin to, empty = session manager picks
};

static struct pipewire_config s_cfg;

static void pipewire_config_defaults(void) {
	s_cfg.enabled = 1;
	s_cfg.capture_sink = 1;
	s_cfg.target[0] = '\0';
}

static int pipewire_config_parse(const char *k, const char *val) {
	if (!strcmp(k, "enabled")) { s_cfg.enabled = atoi(val); return 1; }
	if (!strcmp(k, "capture_sink")) { s_cfg.capture_sink = atoi(val); return 1; }
	if (!strcmp(k, "target")) {
		snprintf(s_cfg.target, sizeof(s_cfg.target), "%s", val);
		return 1;
	}
	return 0;
}

static void sleep_ms(long ms) {
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* Staging ring. Absorbs socket backpressure only, so it stays small. */
#define STAGE_FRAMES (AUDIO_CHUNK_FRAMES * 32)

static float s_stage[STAGE_FRAMES * AUDIO_CHANNELS];
static atomic_uint_least64_t s_stage_head = 0; // producer only
static atomic_uint_least64_t s_stage_tail = 0; // consumer only

/* Producer drops on overrun so the consumer keeps tail. */
static void stage_write(const float *src, uint32_t frames) {
	uint64_t head = atomic_load_explicit(&s_stage_head, memory_order_relaxed);
	uint64_t tail = atomic_load_explicit(&s_stage_tail, memory_order_acquire);

	uint64_t space = STAGE_FRAMES - (head - tail);
	if (frames > space) frames = (uint32_t)space;
	if (!frames) return;

	for (uint32_t i = 0; i < frames; i++) {
		size_t off = (size_t)(head % STAGE_FRAMES) * AUDIO_CHANNELS;
		for (int c = 0; c < AUDIO_CHANNELS; c++)
			s_stage[off + c] = src[i * AUDIO_CHANNELS + c];
		head++;
	}
	atomic_store_explicit(&s_stage_head, head, memory_order_release);
}

static int stage_read_chunk(float *dst) {
	uint64_t head = atomic_load_explicit(&s_stage_head, memory_order_acquire);
	uint64_t tail = atomic_load_explicit(&s_stage_tail, memory_order_relaxed);

	if (head - tail < (uint64_t)AUDIO_CHUNK_FRAMES) return 0;

	for (int n = 0; n < AUDIO_CHUNK_FRAMES; n++) {
		size_t off = (size_t)(tail % STAGE_FRAMES) * AUDIO_CHANNELS;
		for (int c = 0; c < AUDIO_CHANNELS; c++)
			dst[n * AUDIO_CHANNELS + c] = s_stage[off + c];
		tail++;
	}
	atomic_store_explicit(&s_stage_tail, tail, memory_order_release);
	return 1;
}

static void stage_reset(void) {
	uint64_t head = atomic_load_explicit(&s_stage_head, memory_order_acquire);
	atomic_store_explicit(&s_stage_tail, head, memory_order_release);
}

static struct pw_thread_loop *s_loop;
static struct pw_stream *s_stream;
static _Atomic int s_format_ok = 0;

/* Consecutive quiet frames, saturating at release threshold. */
static _Atomic uint64_t s_silent_frames = SILENCE_RELEASE_FRAMES;

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
	(void)data;
	if (!param || id != SPA_PARAM_Format) return;

	struct spa_audio_info info;
	memset(&info, 0, sizeof(info));
	if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0) return;
	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw) return;
	if (spa_format_audio_raw_parse(param, &info.info.raw) < 0) return;

	/* Ask for 48k stereo f32. The graph inserts a converter, but it is
	 * allowed to hand back a different channel count. */
	if (info.info.raw.format != SPA_AUDIO_FORMAT_F32 ||
	    info.info.raw.channels != AUDIO_CHANNELS) {
		fprintf(stderr, "[pipewire] unusable format: %u ch (want %d), not feeding\n",
		        info.info.raw.channels, AUDIO_CHANNELS);
		atomic_store(&s_format_ok, 0);
		return;
	}

	atomic_store(&s_format_ok, 1);
	DBG("[pipewire] negotiated %uHz %uch f32",
	    info.info.raw.rate, info.info.raw.channels);
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
	(void)data; (void)old;
	DBG("[pipewire] stream state: %s%s%s", pw_stream_state_as_string(state),
	    error ? " - " : "", error ? error : "");
	if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
		atomic_store(&s_format_ok, 0);
}

static void on_process(void *data) {
	(void)data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(s_stream);
	if (!b) return;

	struct spa_buffer *buf = b->buffer;
	const float *src = buf->datas[0].data;

	if (src && atomic_load_explicit(&s_format_ok, memory_order_relaxed)) {
		uint32_t stride = AUDIO_CHANNELS * sizeof(float);
		uint32_t frames = buf->datas[0].chunk->size / stride;

		src = (const float *)((const char *)src + buf->datas[0].chunk->offset);

		/* Cheap signal test */
		int loud = 0;
		for (uint32_t i = 0; i < frames * AUDIO_CHANNELS; i++) {
			float v = src[i];
			if (v > 1.0e-4f || v < -1.0e-4f) { loud = 1; break; }
		}

		if (loud) {
			atomic_store_explicit(&s_silent_frames, 0, memory_order_relaxed);
		} else {
			uint64_t q = atomic_load_explicit(&s_silent_frames,
			                                  memory_order_relaxed);
			if (q < SILENCE_RELEASE_FRAMES)
				atomic_store_explicit(&s_silent_frames, q + frames,
				                      memory_order_relaxed);
		}

		if (frames) stage_write(src, frames);
	}

	pw_stream_queue_buffer(s_stream, b);
}

static const struct pw_stream_events s_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static pthread_t s_sender;
static _Atomic int s_run = 0;
static int s_sock = -1;

/* Tracks whether `pw_init` actually ran, so `pw_deinit` stays paired. */
static int s_pw_initialized = 0;

static int feed_connect(void) {
	const char *addr = NULL;
	int port = 0;
	audio_endpoint(&addr, &port);
	if (!addr || port <= 0 || port > 65535) return -1;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) { close(fd); return -1; }

	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		/* A producer already owns the listener. That feed wins.
		 * Retry quietly and take over when it leaves. */
		close(fd);
		return -1;
	}

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

/* Blocking write of the whole buffer. */
static int send_all(int fd, const void *buf, size_t len) {
	const char *p = buf;
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) continue;
			return 0;
		}
		if (n == 0) return 0;
		sent += (size_t)n;
	}
	return 1;
}

static void feed_close(const char *why) {
	DBG("[pipewire] %s", why);
	close(s_sock);
	s_sock = -1;
}

static void *sender_thread(void *arg) {
	(void)arg;

	const size_t chunk_bytes = AUDIO_CHUNK_FRAMES * AUDIO_CHANNELS * sizeof(float);
	float *chunk = malloc(chunk_bytes);
	if (!chunk) return NULL;

	while (atomic_load(&s_run)) {
		if (atomic_load(&s_silent_frames) >= SILENCE_RELEASE_FRAMES) {
			if (s_sock >= 0) feed_close("silent, releasing socket");
			stage_reset();
			sleep_ms(100);
			continue;
		}

		if (s_sock < 0) {
			s_sock = feed_connect();
			if (s_sock < 0) { sleep_ms(500); continue; }
			stage_reset(); // drop whatever went stale while disconnected
			DBG("[pipewire] feeding the local listener");
		}

		if (!stage_read_chunk(chunk)) { sleep_ms(2); continue; }

		if (!send_all(s_sock, chunk, chunk_bytes))
			feed_close("listener went away");
	}

	free(chunk);
	return NULL;
}

/* Check if there is a PipeWire daemon socket to talk to. */
static int pipewire_socket_present(void) {
	const char *dir = getenv("PIPEWIRE_RUNTIME_DIR");
	if (!dir || !dir[0]) dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !dir[0]) return 1; // cannot tell, so try

	const char *name = getenv("PIPEWIRE_REMOTE");
	if (!name || !name[0]) name = "pipewire-0";

	char path[512];
	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return 1;

	return access(path, F_OK) == 0;
}

static int pipewire_init(struct rt *rt) {
	(void)rt;

	if (!s_cfg.enabled) {
		DBG("[pipewire] disabled by config");
		return 1;
	}

	if (!pipewire_socket_present()) {
		DBG("[pipewire] no daemon socket, continuing without capture");
		return 1;
	}

	pw_init(NULL, NULL);
	s_pw_initialized = 1;

	s_loop = pw_thread_loop_new("audio-capture", NULL);
	if (!s_loop) {
		fprintf(stderr, "[pipewire] cannot create loop, continuing without capture\n");
		return 1;
	}

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Music",
		PW_KEY_NODE_NAME, "visualizer-capture",
		NULL);
	if (!props) {
		pw_thread_loop_destroy(s_loop);
		s_loop = NULL;
		return 1;
	}

	/* Sit on the default monitor rather than the default source. */
	if (s_cfg.capture_sink)
		pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

	/* Pin to one node instead of resolving the default. */
	if (s_cfg.target[0])
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, s_cfg.target);

	s_stream = pw_stream_new_simple(pw_thread_loop_get_loop(s_loop),
	                                "visualizer-capture", props,
	                                &s_stream_events, NULL);
	if (!s_stream) {
		fprintf(stderr, "[pipewire] cannot create stream, continuing without capture\n");
		pw_thread_loop_destroy(s_loop);
		s_loop = NULL;
		return 1;
	}

	uint8_t pod_buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
	struct spa_audio_info_raw raw = {
		.format = SPA_AUDIO_FORMAT_F32,
		.rate = AUDIO_SAMPLE_RATE,
		.channels = AUDIO_CHANNELS,
	};
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

	int rc = pw_stream_connect(s_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
	                           PW_STREAM_FLAG_AUTOCONNECT |
	                           PW_STREAM_FLAG_MAP_BUFFERS |
	                           PW_STREAM_FLAG_RT_PROCESS,
	                           params, 1);
	if (rc < 0) {
		fprintf(stderr, "[pipewire] connect failed: %s\n", spa_strerror(rc));
		pw_stream_destroy(s_stream);
		s_stream = NULL;
		pw_thread_loop_destroy(s_loop);
		s_loop = NULL;
		return 1;
	}

	if (pw_thread_loop_start(s_loop) < 0) {
		fprintf(stderr, "[pipewire] cannot start loop, continuing without capture\n");
		pw_stream_destroy(s_stream);
		s_stream = NULL;
		pw_thread_loop_destroy(s_loop);
		s_loop = NULL;
		return 1;
	}

	atomic_store(&s_run, 1);
	if (pthread_create(&s_sender, NULL, sender_thread, NULL) != 0) {
		fprintf(stderr, "[pipewire] cannot start sender thread\n");
		atomic_store(&s_run, 0);
		pw_thread_loop_stop(s_loop);
		pw_stream_destroy(s_stream);
		s_stream = NULL;
		pw_thread_loop_destroy(s_loop);
		s_loop = NULL;
		return 1;
	}

	DBG("[pipewire] capturing the %s", s_cfg.capture_sink
            ? "default sink monitor" : "default source");
	return 1;
}

static void pipewire_shutdown(void) {
	if (atomic_load(&s_run)) {
		atomic_store(&s_run, 0);
		pthread_join(s_sender, NULL);
	}
	if (s_sock >= 0) { close(s_sock); s_sock = -1; }

	if (s_loop) pw_thread_loop_stop(s_loop);
	if (s_stream) { pw_stream_destroy(s_stream); s_stream = NULL; }
	if (s_loop) { pw_thread_loop_destroy(s_loop); s_loop = NULL; }

	if (s_pw_initialized) { pw_deinit(); s_pw_initialized = 0; }
}

MODULE_REGISTER(pipewire,
	.init = pipewire_init,
	.shutdown = pipewire_shutdown,
	.config_prefix = "pipewire",
	.config_template =
		"pipewire.enabled=1   # bool\n"
		"pipewire.capture_sink=1   # 1 = active pipewire audio. 0 = input device\n"
		"pipewire.target=   # node.name to attach. empty = session default\n",
	.config_defaults = pipewire_config_defaults,
	.config_parse = pipewire_config_parse);
