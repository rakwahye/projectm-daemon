#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "module_registry.h"
#include "playlist.h"
#include "audio.h"
#include "info_views.h"
#include "visualizer.h"
#include "runtime.h"
#include "wiring_render.h"

#include "check.h"

/* Every symbol the module reaches is stubbed below, so a sideways reach into
 * an unstubbed peer fails to link and the link is itself the boundary check.
 * The real headers are included above so the stub signatures are checked
 * against the declarations. */
int g_debug = 0;
_Atomic sig_atomic_t g_running = 1;

static struct {
	int    should_skip;
	int    audio_frames;
	int    transition_in_progress;
	double transition_elapsed;
	double soft_cut;
	int    playlist_count;
	int    is_locked;
	double preset_age;
	int    should_auto_advance;
	int    advance_request;
	int    next_calls;
	int    load_current_calls;
	int    last_load_smooth;
	int    mark_dirty_calls;
	int    apply_pending_calls;
	int    frames_rendered;
} S;

static void reset_stubs(void) {
	memset(&S, 0, sizeof(S));
	g_running = 1;
	g_debug = 0;
}

static void tp_poll(void)        { }
static int  tp_should_skip(void) { return S.should_skip; }
static void tp_observe(int t)    { (void)t; }
static void tp_presented(void)   { S.frames_rendered++; }

static const struct frame_pacer test_pacer = {
	.poll        = tp_poll,
	.should_skip = tp_should_skip,
	.observe     = tp_observe,
	.presented   = tp_presented,
};

const struct frame_pacer *module_active_pacer(void) { return &test_pacer; }

int audio_read(float *out, int max_frames) { (void)out; (void)max_frames;
											 return S.audio_frames; }

/* The visualizer is reached through rt->vis, not as a direct symbol, so a fake
 * instance is attached to each rt below rather than stubbed here. */
static void   fake_feed_pcm(struct visualizer *v, const float *data, size_t frames)
			  { (void)v; (void)data; (void)frames; }
static double fake_soft_cut_duration(struct visualizer *v) { (void)v; return S.soft_cut; }

static struct visualizer g_fake_vis = {
	.feed_pcm          = fake_feed_pcm,
	.soft_cut_duration = fake_soft_cut_duration,
};

int    playlist_transition_in_progress(void)     { return S.transition_in_progress; }
double playlist_transition_elapsed_seconds(void) { return S.transition_elapsed; }
const char *playlist_transition_text(void)       { return "stub-preset"; }
void   playlist_clear_transition(void)           { S.transition_in_progress = 0; }
int    playlist_count(void)               { return S.playlist_count; }
int    playlist_is_locked(void)           { return S.is_locked; }
double playlist_preset_age_seconds(void)  { return S.preset_age; }
bool   playlist_should_auto_advance(void) { return S.should_auto_advance; }
void   playlist_next(void)                { S.next_calls++; }
void   playlist_load_current(bool smooth) { S.last_load_smooth = smooth ? 1 : 0; S.load_current_calls++; }
int    playlist_poll_advance(void) { int v = S.advance_request; S.advance_request = 0; return v; }
void   playlist_mark_dirty(void)          { S.mark_dirty_calls++; }

void   info_preset(const char *text) { (void)text; }
void   info_views_tick(void) { }

void   apply_pending(struct rt *rt)       { (void)rt; S.apply_pending_calls++; }

int main(void) {
	reset_stubs();
	struct rt a = { .frame_count = 0, .vis = &g_fake_vis };
	struct rt b = { .frame_count = 0, .vis = &g_fake_vis };
	wiring_render_epilogue(&a);
	wiring_render_epilogue(&a);
	wiring_render_epilogue(&a);
	wiring_render_epilogue(&b);
	CHECK(a.frame_count == 3, "the epilogue counts frames into the rt it was given");
	CHECK(b.frame_count == 1, "a second rt counts independently, so the count is not a hidden static");
	CHECK(S.frames_rendered == 4, "pacer bookkeeping fires on every frame regardless of which rt");

	/* The decision itself belongs to the playlist and is tested there. What is
	 * proved here is only that the prologue acts on the answer it gets. */
	reset_stubs();
	S.should_skip = 0;
	S.audio_frames = 0;
	S.transition_in_progress = 0;
	S.should_auto_advance = 1;

	struct rt rt = { .frame_count = 0, .vis = &g_fake_vis };
	bool rendered = wiring_render_prologue(&rt);
	CHECK(rendered, "the prologue renders the frame");
	CHECK(S.next_calls == 1, "the prologue advances when the playlist says to");
	CHECK(S.load_current_calls == 1, "an auto-advance loads the new preset");
	CHECK(S.mark_dirty_calls == 1, "an auto-advance marks the snapshot dirty");

	reset_stubs();
	S.should_auto_advance = 0;
	struct rt rt_off = { .frame_count = 0, .vis = &g_fake_vis };
	wiring_render_prologue(&rt_off);
	CHECK(S.next_calls == 0, "the prologue does not advance when the playlist says not to");

	/* The engine asks for an advance from inside its own render. Servicing it
	 * here, before the scene renders, keeps a cross-engine swap off the
	 * engine's own stack. */
	reset_stubs();
	S.should_auto_advance = 0;
	S.advance_request = 2;
	S.is_locked = 0;
	struct rt rt_ea = { .frame_count = 0, .vis = &g_fake_vis };
	wiring_render_prologue(&rt_ea);
	CHECK(S.next_calls == 1, "the prologue services an advance the engine requested");
	CHECK(S.load_current_calls == 1, "an engine advance loads the new preset");
	CHECK(S.last_load_smooth == 0, "a hard-cut request loads as a snap, not a transition");
	CHECK(S.mark_dirty_calls == 1, "an engine advance marks the snapshot dirty");

	reset_stubs();
	S.advance_request = 1;
	struct rt rt_soft = { .frame_count = 0, .vis = &g_fake_vis };
	wiring_render_prologue(&rt_soft);
	CHECK(S.last_load_smooth == 1, "a soft request loads as a smooth transition");

	reset_stubs();
	S.advance_request = 2;
	S.is_locked = 1;
	struct rt rt_lock = { .frame_count = 0, .vis = &g_fake_vis };
	wiring_render_prologue(&rt_lock);
	CHECK(S.next_calls == 0, "a lock suppresses the engine's advance request");

	reset_stubs();
	g_running = 0;
	struct rt rt_stop = { .frame_count = 0, .vis = &g_fake_vis };
	bool p = wiring_render_prologue(&rt_stop);
	CHECK(!p, "the prologue short-circuits once the daemon is no longer running");

	CHECK_PASS("test-wiring_render");
}
