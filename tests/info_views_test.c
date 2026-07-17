#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "module_registry.h"
#include "info_views.h"
#include "overlay.h"
#include "playlist.h"

#include "check.h"

/* The overlay and playlist symbols the verb handler reaches are stubbed. The
 * overlay ones record what was composed, so the round-trip can be checked
 * without a live overlay. */
static char g_peek[2048];
static char g_view[4096];
static int  g_view_show_art;

void overlay_show_info(const char *text, int show_art) {
	(void)show_art;
	snprintf(g_peek, sizeof(g_peek), "%s", text ? text : "");
}

void overlay_set_view(const char *text, int show_art) {
	snprintf(g_view, sizeof(g_view), "%s", text ? text : "");
	g_view_show_art = show_art;
}

void overlay_set_art(const char *art_path) { (void)art_path; }

void playlist_snapshot_get(struct playlist_view *out) {
	if (out) memset(out, 0, sizeof(*out));
}

double playlist_auto_advance_seconds(void) { return 0.0; }

static const struct module_descriptor *g_info = NULL;

static void find_info(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "info") == 0) g_info = d;
}

int main(void) {
	module_registry_visit(find_info, NULL);
	CHECK(g_info != NULL, "info_views registers a descriptor under prefix info");
	CHECK(g_info->ipc_verb != NULL, "info_views registers an IPC verb");
	CHECK(strcmp(g_info->ipc_verb, "info") == 0, "the IPC verb is spelled info");
	CHECK(g_info->config_defaults != NULL, "info_views supplies a defaults hook");
	CHECK(g_info->config_parse != NULL, "info_views supplies a parse hook");

	g_info->config_defaults();
	CHECK(g_info->config_parse("peek", "art,nowplaying,preset") == 1, "parse claims peek with a field list");
	CHECK(g_info->config_parse("peek", "none") == 1, "peek accepts none, which selects no fields");
	CHECK(g_info->config_parse("bogus", "x") == 0, "parse rejects an unknown key");

	CHECK(g_info->config_parse("peek", "preset") == 1, "peek accepts a single field");
	info_preset("xyzzy-preset");
	char reply[8192];
	struct ipc_command_ctx cc = { .reply = reply, .reply_len = sizeof(reply) };
	CHECK(g_info->ipc_command(&cc) == 0, "the info verb runs and composes a peek");
	CHECK(strstr(g_peek, "xyzzy-preset") != NULL, "a published field appears in the composed peek");

	CHECK(g_info->config_parse("preset_mode", "2") == 1, "preset_mode accepts the persistent mode");
	info_preset("hud-preset");
	info_views_tick();
	CHECK(strstr(g_view, "hud-preset") != NULL, "a persistent field reaches the HUD on the frame tick");

	CHECK_PASS("test-info_views");
}
