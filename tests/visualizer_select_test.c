#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "visualizer.h"
#include "module_registry.h"

#include "check.h"

static char g_log[64][32];
static int  g_log_n;

static void logev(const char *ev) {
	if (g_log_n < 64) { snprintf(g_log[g_log_n], 32, "%s", ev); g_log_n++; }
}

static void log_reset(void) { g_log_n = 0; }

static bool eng_init(struct visualizer *v, int w, int h) {
	(void)v; (void)w; (void)h;
	logev("init");
	return true;
}

static void eng_destroy(struct visualizer *v) { (void)v; logev("destroy"); }

static struct visualizer s_visA = { .init = eng_init, .destroy = eng_destroy };
static struct visualizer s_visB = { .init = eng_init, .destroy = eng_destroy };
static struct visualizer s_visN = { .init = eng_init, .destroy = eng_destroy };

static struct visualizer *createA(void) { logev("createA"); return &s_visA; }
static struct visualizer *createB(void) { logev("createB"); return &s_visB; }
static struct visualizer *createN(void) { logev("createN"); return &s_visN; }

/* Registered here so the real resolver walks them through the linker section.
 * The engine with no file_extensions is the fallback. */
MODULE_REGISTER(engA, .create = createA, .file_extensions = ".aaa .bbb");
MODULE_REGISTER(engB, .create = createB, .file_extensions = ".ccc");
MODULE_REGISTER(fallback, .create = createN);

/* The only stubs: the runtime pointer accessors, because the module that owns
 * the real runtime is not in this link set. Everything else is shipping code. */
static struct visualizer               *s_active;
static const struct module_descriptor  *s_active_desc;

struct visualizer *visualizer_active(void)                  { return s_active; }
const struct module_descriptor *visualizer_active_desc(void){ return s_active_desc; }

void visualizer_set_active(struct visualizer *v, const struct module_descriptor *d) {
	s_active = v; s_active_desc = d;
	logev(v ? "publish" : "clear");
}

void visualizer_output_size(int *w, int *h) { if (w) *w = 640; if (h) *h = 480; }

#define CHECK_LOG(i, s, why) CHECK(strcmp(g_log[(i)], (s)) == 0, why)

int main(void) {
	const struct module_descriptor *dA = module_engine_for("x.aaa");
	const struct module_descriptor *dB = module_engine_for("x.ccc");
	const struct module_descriptor *dN = module_engine_for("x.zzz");

	CHECK(dA != NULL && dA->create == createA, "a suffix resolves to the engine that advertised it");
	CHECK(dB != NULL && dB->create == createB, "a second engine resolves to itself, not the first");
	CHECK(dN != NULL && dN->create == createN, "an unadvertised suffix resolves to the engine with no extensions");
	CHECK(module_engine_for("x.bbb") == dA, "an engine's second advertised suffix resolves to it");
	CHECK(module_engine_for("X.BBB") == dA, "suffix matching is case-insensitive");
	CHECK(module_engine_for("song.CCC") == dB, "case-insensitivity holds for every engine");
	CHECK(module_engine_for("weird.name") == dN, "an unknown suffix falls back rather than failing");
	CHECK(dA != dB && dA != dN && dB != dN, "the three engines are distinct descriptors");

	s_active = NULL; s_active_desc = NULL; log_reset();
	struct visualizer *got = visualizer_ensure_for("track.aaa");
	CHECK(got == &s_visA, "starting from nothing brings up the engine the item resolves to");
	CHECK(g_log_n == 3, "a first start does exactly three things, so nothing is torn down");
	CHECK_LOG(0, "createA", "a first start creates the instance");
	CHECK_LOG(1, "init", "a first start inits after creating");
	CHECK_LOG(2, "publish", "a first start publishes only once the engine is inited");
	CHECK(s_active == &s_visA && s_active_desc == dA, "the published instance and descriptor agree");

	log_reset();
	got = visualizer_ensure_for("other.bbb");
	CHECK(got == &s_visA, "an item resolving to the active engine reuses the live instance");
	CHECK(g_log_n == 0, "staying on the same engine is a pure no-op, with no teardown or bring-up");
	CHECK(s_active == &s_visA && s_active_desc == dA, "a no-op leaves the published state alone");

	log_reset();
	got = visualizer_ensure_for("clip.ccc");
	CHECK(got == &s_visB, "an item resolving to another engine switches to it");
	CHECK(g_log_n == 5, "a switch does exactly five things");
	CHECK_LOG(0, "destroy", "a switch tears the old engine down before anything else");
	CHECK_LOG(1, "clear", "a switch clears the slot before bringing the new engine up");
	CHECK_LOG(2, "createB", "the new instance is created only after the old one is gone");
	CHECK_LOG(3, "init", "the new instance is inited after creation");
	CHECK_LOG(4, "publish", "the new instance is published last");
	CHECK(s_active == &s_visB && s_active_desc == dB, "the switch publishes the new instance and descriptor");

	log_reset();
	got = visualizer_ensure_for("garbage.qux");
	CHECK(got == &s_visN, "an unknown suffix switches to the fallback engine");
	CHECK_LOG(0, "destroy", "switching to the fallback still tears the old engine down first");
	CHECK_LOG(2, "createN", "switching to the fallback creates the fallback instance");
	CHECK(s_active == &s_visN && s_active_desc == dN, "the fallback is published like any other engine");

	CHECK_PASS("test-visualizer-select");
}
