#include <stdio.h>
#include <string.h>

#include "backend.h"
#include "module_registry.h"

#include "check.h"

static const struct module_descriptor *g_d = NULL;

static void find_d(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "display") == 0) g_d = d;
}

int main(void) {
	module_registry_visit(find_d, NULL);
	CHECK(g_d != NULL, "backend registers a descriptor under prefix display");
	CHECK(g_d->config_defaults != NULL, "backend supplies a defaults hook");
	CHECK(g_d->config_parse != NULL, "backend supplies a parse hook");

	g_d->config_defaults();
	CHECK(strcmp(backend_mode(), "auto") == 0, "default mode is auto");

	CHECK(g_d->config_parse("mode", "windowed") == 1, "parse claims the mode key");
	CHECK(strcmp(backend_mode(), "windowed") == 0, "accessor reports the parsed mode");

	CHECK(g_d->config_parse("bogus", "x") == 0, "parse rejects an unknown key");
	CHECK(g_d->config_parse("layer", "1") == 0, "parse rejects layer, which another module owns");

	CHECK_PASS("test-backend");
}
