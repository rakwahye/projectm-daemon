#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "module_registry.h"

#include "check.h"

/* Ambient in the real build. Defined here because the owner is not linked. */
int g_debug = 0;

static const struct module_descriptor *g_audio = NULL;

static void find_audio(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "audio") == 0)
		g_audio = d;
}

int main(void) {
	module_registry_visit(find_audio, NULL);
	CHECK(g_audio != NULL, "audio registers a descriptor under prefix audio");
	CHECK(g_audio->config_defaults != NULL, "audio supplies a defaults hook");
	CHECK(g_audio->config_parse != NULL, "audio supplies a parse hook");
	CHECK(g_audio->shutdown != NULL, "audio registers a shutdown hook, so the socket closes");

	g_audio->config_defaults();

	/* The slice storage is private, so the contract here is the parser's
	 * claim and reject. The value round-trip is covered by the daemon smoke. */
	CHECK(g_audio->config_parse("port", "9100") == 1, "parse claims the port key");
	CHECK(g_audio->config_parse("addr", "127.0.0.1") == 1, "parse claims the addr key");
	CHECK(g_audio->config_parse("bogus", "x") == 0, "parse rejects an unknown key");
	CHECK(g_audio->config_parse("rate", "48000") == 0, "parse rejects rate, which is not an audio key");

	CHECK_PASS("test-audio");
}
