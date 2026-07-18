#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "filter.h"
#include "module_registry.h"
#include "gl_quad.h"

#include "check.h"

/* Stubbed, never called. Included so the signature is checked against the
 * declaration, and so a reach into the real GL module fails to link. */
void gl_quad_tint(float r, float g, float b, float a) { (void)r;(void)g;(void)b;(void)a; }

static const struct module_descriptor *g_filter = NULL;

static void find_filter(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "filter") == 0) g_filter = d;
}

int main(void) {
	module_registry_visit(find_filter, NULL);
	CHECK(g_filter != NULL, "filter registers a descriptor under prefix filter");
	CHECK(g_filter->config_defaults != NULL, "filter supplies a defaults hook");
	CHECK(g_filter->config_parse != NULL, "filter supplies a parse hook");

	g_filter->config_defaults();
	CHECK(g_filter->config_parse("tint", "112233ff") == 1, "parse claims the tint key");
	CHECK(g_filter->config_parse("bogus", "x") == 0, "parse rejects an unknown key");
	CHECK(g_filter->config_parse("bg_color", "0") == 0, "parse rejects bg_color, which another module owns");

	CHECK_PASS("test-filter");
}
