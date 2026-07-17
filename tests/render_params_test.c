#include <stdio.h>
#include <string.h>

#include "render_params.h"
#include "module_registry.h"

#include "check.h"

static const struct module_descriptor *g_rp = NULL;

static void find_rp(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "render") == 0) g_rp = d;
}

int main(void) {
	module_registry_visit(find_rp, NULL);
	CHECK(g_rp != NULL, "render_params registers a descriptor under prefix render");
	CHECK(g_rp->config_defaults != NULL, "render_params supplies a defaults hook");
	CHECK(g_rp->config_parse != NULL, "render_params supplies a parse hook");

	g_rp->config_defaults();
	CHECK(render_mesh_x() == 48, "default mesh_x is 48");
	CHECK(render_mesh_y() == 32, "default mesh_y is 32");
	CHECK(render_fps()    == 60, "default fps is 60");

	CHECK(g_rp->config_parse("mesh_x", "64") == 1, "parse claims the mesh_x key");
	CHECK(g_rp->config_parse("mesh_y", "48") == 1, "parse claims the mesh_y key");
	CHECK(g_rp->config_parse("fps",    "30") == 1, "parse claims the fps key");
	CHECK(g_rp->config_parse("bogus",  "1")  == 0, "parse rejects an unknown key");
	CHECK(g_rp->config_parse("bg_color", "0") == 0, "parse rejects bg_color, which another module owns");

	CHECK(render_mesh_x() == 64, "accessor reports the parsed mesh_x");
	CHECK(render_mesh_y() == 48, "accessor reports the parsed mesh_y");
	CHECK(render_fps()    == 30, "accessor reports the parsed fps");

	CHECK_PASS("test-render_params");
}
