#include <stdio.h>
#include <stdint.h>

#include "scene_router.h"
#include "visualizer.h"
#include "gl_quad.h"

#include "check.h"

/* The router's only two deps: the visualizer vtable, faked below, and the
 * quad blit, stubbed here to record its calls. A reach past either fails
 * to link. */
static int g_composite_calls;
static int g_last_composite_x, g_last_composite_w, g_last_composite_sw;
static float g_last_composite_alpha;

void gl_quad_blit(GLuint tex, int x, int y, int w, int h,
					 int surf_w, int surf_h, float alpha) {
	(void)tex; (void)y; (void)h; (void)surf_h;
	g_composite_calls++;
	g_last_composite_x = x;
	g_last_composite_w = w;
	g_last_composite_sw = surf_w;
	g_last_composite_alpha = alpha;
}

static int g_deposit_calls;
static int g_last_deposit_x, g_last_deposit_w;

static void fake_deposit(struct visualizer *v, uint32_t tex,
					   int x, int y, int w, int h) {
	(void)v; (void)tex; (void)y; (void)h;
	g_deposit_calls++;
	g_last_deposit_x = x;
	g_last_deposit_w = w;
}

int main(void) {
	struct visualizer no_deposit = {0};
	g_composite_calls = g_deposit_calls = 0;
	scene_router_deposit(&no_deposit, /*tex*/42, /*x*/10, /*y*/20, /*w*/30, /*h*/40,
					  /*surf_w*/640, /*surf_h*/480, /*alpha*/0.5f);
	CHECK(g_composite_calls == 1, "a visualizer with no deposit op falls back to compositing");
	CHECK(g_deposit_calls == 0, "the fallback does not also deposit");
	CHECK(g_last_composite_x == 10, "the intent rect origin reaches the quad");
	CHECK(g_last_composite_w == 30, "the intent rect size reaches the quad");
	CHECK(g_last_composite_sw == 640, "the surface size reaches the quad");
	CHECK(g_last_composite_alpha == 0.5f, "alpha is applied when compositing");

	struct visualizer with_deposit = {0};
	with_deposit.deposit = fake_deposit;
	g_composite_calls = g_deposit_calls = 0;
	scene_router_deposit(&with_deposit, /*tex*/7, /*x*/11, /*y*/22, /*w*/33, /*h*/44,
					  /*surf_w*/640, /*surf_h*/480, /*alpha*/0.5f);
	CHECK(g_deposit_calls == 1, "a visualizer advertising deposit gets the deposit");
	CHECK(g_composite_calls == 0, "depositing does not also composite");
	CHECK(g_last_deposit_x == 11, "the intent rect origin reaches the deposit op");
	CHECK(g_last_deposit_w == 33, "the intent rect size reaches the deposit op");

	CHECK_PASS("test-scene_router");
}
