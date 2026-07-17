#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "cli.h"
#include "module_registry.h"

#include "check.h"

/* Registered through the linker set, so the registry walk inside cli_parse
 * exercises the register_cli hook end to end. */
static bool s_fake_registered = false;

static int fake_register_cli(void)
{
	static const struct cli_option opts[] = {
		{ .long_name = "fps-max", .short_name = 0, .has_arg = CLI_REQUIRED_ARG,
		  .help = "cap fps", .config_key = "throttle.fps_max", .lo = 0, .hi = 1000 },
	};
	s_fake_registered = true;
	return cli_register("fake-throttle", opts, 1);
}
MODULE_REGISTER(faketh, .register_cli = fake_register_cli);

static bool s_verbose = false;
static char s_name[32] = "";
static int  h_verbose(const char *a) { (void)a; s_verbose = true; return 0; }
static int  h_name(const char *a)    { snprintf(s_name, sizeof(s_name), "%s", a ? a : ""); return 0; }

/* Overrides route through a sink the caller installs, so cli carries no peer
 * dependency. This one records instead of applying. */
static char s_kv_key[64] = "", s_kv_val[32] = "";

static void rec_apply_kv(const char *k, const char *v)
{
	snprintf(s_kv_key, sizeof(s_kv_key), "%s", k);
	snprintf(s_kv_val, sizeof(s_kv_val), "%s", v);
}

int main(void)
{
	static const struct cli_option core[] = {
		{ .long_name = "verbose", .short_name = 'v', .has_arg = CLI_NO_ARG,
		  .help = "verbose", .handler = h_verbose },
		{ .long_name = "name", .short_name = 'n', .has_arg = CLI_REQUIRED_ARG,
		  .help = "a name", .handler = h_name },
	};
	CHECK(cli_register("core", core, 2) == 0, "a non-module group registers directly");
	cli_set_override_sink(rec_apply_kv);

	char *av1[] = { "prog", "--verbose", "-n", "hello", "--fps-max", "240", NULL };
	bool ok = cli_parse(6, av1);
	CHECK(ok, "a well-formed argv parses");
	CHECK(s_fake_registered, "the registry walk invoked the fake module's register_cli");
	CHECK(s_verbose, "a no-arg long option fires its handler");
	CHECK(strcmp(s_name, "hello") == 0, "a short option captures its argument");
	CHECK(s_kv_key[0] == '\0', "a sticky override is held back until it is applied");

	cli_apply_overrides(false);
	CHECK(strcmp(s_kv_key, "throttle.fps_max") == 0, "applying routes the override under its config key");
	CHECK(strcmp(s_kv_val, "240") == 0, "applying routes the override's value");

	char *av2[] = { "prog", "--fps-max", "5000", NULL };
	CHECK(cli_parse(3, av2) == false, "a sticky value outside the option's range is rejected");

	CHECK_PASS("test-cli");
}
