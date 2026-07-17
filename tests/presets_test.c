#include <stdio.h>
#include <string.h>

#include "presets.h"
#include "module_registry.h"

#include "check.h"

static const struct module_descriptor *g_p = NULL;

static void find_p(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "presets") == 0) g_p = d;
}

int main(void) {
	module_registry_visit(find_p, NULL);
	CHECK(g_p != NULL, "presets registers a descriptor under prefix presets");
	CHECK(g_p->config_defaults != NULL, "presets supplies a defaults hook");
	CHECK(g_p->config_parse != NULL, "presets supplies a parse hook");

	/* No visualizer is linked here, so nothing advertises an extension and
	 * the union starts empty. The concrete root comes from the build config. */
	g_p->config_defaults();
	CHECK(presets_dir()[0] == '\0', "the default root is empty until the build config sets one");
	CHECK(presets_dir_count() == 0, "an empty default root yields no roots");
	CHECK(preset_extensions()[0] == '\0', "with no engine linked, the advertised union is empty");
	CHECK(preset_ext_match("clip.xyz") == 0, "an empty extension set matches nothing");

	CHECK(g_p->config_parse("dir", "/tmp/presets/") == 1, "parse claims the dir key");
	CHECK(presets_dir_count() == 1, "one root parses to one root");
	CHECK(strcmp(presets_dir_at(0), "/tmp/presets") == 0, "a trailing slash is stripped from a root");

	CHECK(g_p->config_parse("dir", "/a:/b/::/c/") == 1, "parse accepts a colon-separated root list");
	CHECK(presets_dir_count() == 3, "empty tokens between colons are skipped");
	CHECK(strcmp(presets_dir_at(0), "/a") == 0, "roots keep their order");
	CHECK(strcmp(presets_dir_at(1), "/b") == 0, "each root is stripped independently");
	CHECK(strcmp(presets_dir_at(2), "/c") == 0, "the last root parses like the rest");
	CHECK(presets_dir_at(3) == NULL, "an index past the last root reads NULL");

	CHECK(g_p->config_parse("extensions", ".foo .bar.baz") == 1, "parse claims the extensions key");
	CHECK(preset_ext_match("clip.foo")  == 1, "a configured suffix matches");
	CHECK(preset_ext_match("a.BAR.BAZ") == 1, "suffix matching is case-insensitive and spans dots");
	CHECK(preset_ext_match("nope.qux")  == 0, "an unconfigured suffix does not match");
	CHECK(g_p->config_parse("extensions", ".zzz") == 1, "a second extensions line parses");
	CHECK(preset_ext_match("song.zzz")  == 1, "the second line's suffix matches");
	CHECK(preset_ext_match("clip.foo")  == 1, "extensions append to the union rather than replace it");

	CHECK(g_p->config_parse("bogus", "x") == 0, "parse rejects an unknown key");
	CHECK(g_p->config_parse("mesh_x", "1") == 0, "parse rejects mesh_x, which another module owns");

	CHECK_PASS("test-presets");
}
