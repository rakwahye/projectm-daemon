#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "scene.h"
#include "module_registry.h"
#include "overlay.h"
#include "gl_quad.h"
#include "scene_router.h"

#include "check.h"

/* Every render-time symbol the module reaches is stubbed below and never
 * called. Real headers are included above so the stub signatures are checked
 * against the declarations, and a reach into an unstubbed peer fails to link. */
int g_debug = 0;
struct visualizer *visualizer_active(void) { return NULL; }
void overlay_tick(void) {}
int  overlay_burn_enabled(void) { return 0; }
int  overlay_poll_burn(uint32_t *t, int *x, int *y, int *w, int *h, float *a)
	 { (void)t;(void)x;(void)y;(void)w;(void)h;(void)a; return 0; }
void overlay_render_present(int w, int h) { (void)w;(void)h; }
void gl_quad_fill_holes(float r, float g, float b, float a) { (void)r;(void)g;(void)b;(void)a; }
void scene_router_deposit(struct visualizer *v, uint32_t tex, int x, int y, int w,
					   int h, int sw, int sh, float a)
	 { (void)v;(void)tex;(void)x;(void)y;(void)w;(void)h;(void)sw;(void)sh;(void)a; }

static const struct module_descriptor *g_scene = NULL;

static void find_scene(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "scene") == 0)
		g_scene = d;
}

int main(void) {
	module_registry_visit(find_scene, NULL);
	CHECK(g_scene != NULL, "scene registers a descriptor under prefix scene");
	CHECK(g_scene->config_defaults != NULL, "scene supplies a defaults hook");
	CHECK(g_scene->config_parse != NULL, "scene supplies a parse hook");

	g_scene->config_defaults();

	/* bg_color has no getter, so the contract here is the parser's claim and
	 * reject. The value round-trip is covered by the daemon smoke. */
	CHECK(g_scene->config_parse("bg_color", "112233ff") == 1, "parse claims the bg_color key");
	CHECK(g_scene->config_parse("bogus", "x") == 0, "parse rejects an unknown key");
	CHECK(g_scene->config_parse("port", "9100") == 0, "parse rejects port, which another module owns");
	CHECK(g_scene->config_parse("burn", "1") == 0, "parse rejects burn, which another module owns");

	CHECK_PASS("test-scene");
}
