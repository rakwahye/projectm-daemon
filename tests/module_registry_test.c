#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "module_registry.h"

#include "check.h"

/* The fake module is registered here in the driver, not in a peer, so the
 * linker-set collection is exercised live rather than only in its empty
 * state. Registration is a dedicated section plus start and stop symbols, so
 * a broken section would still pass an empty-state-only test. */
static int g_fake_init_called     = 0;
static int g_fake_defaults_called = 0;

static int fake_init(struct rt *rt) {
	(void)rt;
	g_fake_init_called++;
	return 1;
}

static void fake_config_defaults(void) {
	g_fake_defaults_called++;
}

static int fake_config_parse(const char *subkey, const char *val) {
	(void)val;
	return strcmp(subkey, "known") == 0 ? 1 : 0;
}

MODULE_REGISTER(test_fake_module,
	.init            = fake_init,
	.config_prefix   = "fake",
	.config_defaults = fake_config_defaults,
	.config_parse    = fake_config_parse);

static int g_visit_count = 0;
static const struct module_descriptor *g_last_visited = NULL;

static void visitor(const struct module_descriptor *d, void *ud) {
	(void)ud;
	g_visit_count++;
	g_last_visited = d;
}

int main(void) {
	CHECK(module_registry_count() == 1, "the count sees the fake module and skips the sentinel");

	module_registry_visit(visitor, NULL);
	CHECK(g_visit_count == 1, "the walk visits each registered module once");
	CHECK(g_last_visited != NULL, "the walk hands the visitor a descriptor");
	CHECK(g_last_visited->name != NULL, "a registered module carries its name");
	CHECK(strcmp(g_last_visited->name, "test_fake_module") == 0, "the name is the one given to MODULE_REGISTER");

	CHECK(g_last_visited->init == fake_init, "an init named in the initializer lands in the init field");
	CHECK(g_last_visited->config_defaults == fake_config_defaults, "a defaults hook lands in its field");
	CHECK(g_last_visited->config_parse == fake_config_parse, "a parse hook lands in its field");
	CHECK(g_last_visited->config_prefix != NULL, "a prefix named in the initializer survives collection");
	CHECK(strcmp(g_last_visited->config_prefix, "fake") == 0, "the prefix is the one given to MODULE_REGISTER");
	CHECK(g_last_visited->shutdown == NULL, "a field left unset reads NULL");
	CHECK(g_last_visited->register_cli == NULL, "every unset field reads NULL, not garbage");

	CHECK(g_last_visited->init((struct rt *)0xCAFE) == 1, "calling through the descriptor reaches the real function");
	CHECK(g_fake_init_called == 1, "the call arrived exactly once");

	CHECK(module_registry_init_all((struct rt *)0xCAFE) == NULL, "the init walk returns NULL when every init succeeds");
	CHECK(g_fake_init_called == 2, "the init walk called the module's init");

	g_last_visited->config_defaults();
	CHECK(g_fake_defaults_called == 1, "calling the defaults hook through the descriptor reaches it");
	CHECK(g_last_visited->config_parse("known", "v") == 1, "a claimed subkey parses through the descriptor");
	CHECK(g_last_visited->config_parse("nope", "v") == 0, "an unclaimed subkey is rejected through the descriptor");

	module_registry_visit(NULL, NULL);
	CHECK(g_visit_count == 1, "a NULL visitor is survivable and calls nothing");

	CHECK_PASS("test-module_registry");
}
