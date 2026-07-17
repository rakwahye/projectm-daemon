#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "module_registry.h"
#include "render_params.h"

#include "check.h"

/* Ambient in the real build. Defined here because the owner is not linked. */
int g_debug = 0;

/* Carries a config_apply hook but no config_parse, so it stays out of the
 * routing checks and only proves the apply walk. */
static int g_fake_applied = 0;
static void fake_apply(void) { g_fake_applied++; }
MODULE_REGISTER(cfgtest_fake,
	.config_prefix = "cfgtest",
	.config_apply  = fake_apply);

int main(void) {
	config_apply_kv("render.mesh_x", "99");
	CHECK(render_mesh_x() == 99, "a dotted key routes to the owning slice's parser");

	/* There is no fallback path: only the dotted form reaches a slice. */
	config_apply_kv("mesh_x", "7");
	CHECK(render_mesh_x() == 99, "a flat key reaches no slice, so the value is unchanged");

	config_apply_kv("render.bogus", "x");
	CHECK(render_mesh_x() == 99, "an unknown subkey under a real prefix leaves the slice alone");

	config_apply_kv("ghost.key", "1");
	CHECK(render_mesh_x() == 99, "a dotted key with no owner is survivable and touches nothing");

	config_apply_all();
	CHECK(g_fake_applied == 1, "the apply walk calls every registered config_apply hook once");

	CHECK_PASS("test-config-router");
}
