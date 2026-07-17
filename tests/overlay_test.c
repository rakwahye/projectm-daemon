#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "overlay.h"
#include "layer.h"
#include "module_registry.h"

#include "check.h"

static const struct module_descriptor *g_overlay = NULL;

static void find_overlay(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "overlay") == 0)
		g_overlay = d;
}

/* Staging is a mailbox write, so a command runs with no GL and no init. */
static int ol_call(const char *const *argv, int argc, char *reply, int len) {
	struct ipc_command_ctx c = {
		.fd = -1, .args = "", .argc = argc, .argv = (char **)(uintptr_t)argv,
		.body = "", .body_len = 0, .output_w = 1920, .output_h = 1080,
		.reply = reply, .reply_len = len,
	};
	return g_overlay->ipc_command(&c);
}

/* The three below drive one overlay command each. The flag names are a frozen
 * user contract and the client no longer validates them, so a typo would be
 * silent. Every name is driven once, which is what guards the freeze. */
#define OL_OK(...) do {                                                      \
	const char *a[] = { "overlay", __VA_ARGS__ };                            \
	char r[256] = {0};                                                       \
	ol_call(a, (int)(sizeof(a) / sizeof(a[0])), r, sizeof(r));               \
	if (strncmp(r, "ok", 2) != 0) fprintf(stderr, "  reply: %s", r);         \
	CHECK(strncmp(r, "ok", 2) == 0, "a frozen overlay flag is still accepted"); \
} while (0)

#define OL_ERR(...) do {                                                     \
	const char *a[] = { "overlay", __VA_ARGS__ };                            \
	char r[256] = {0};                                                       \
	ol_call(a, (int)(sizeof(a) / sizeof(a[0])), r, sizeof(r));               \
	if (strncmp(r, "err", 3) != 0) fprintf(stderr, "  reply: %s", r);        \
	CHECK(strncmp(r, "err", 3) == 0, "a malformed overlay command is refused"); \
} while (0)

#define OL_PARSE(d, ...) do {                                                \
	const char *a[] = { "overlay", __VA_ARGS__ };                            \
	char e[192] = {0};                                                       \
	int ok = overlay_spec_parse((int)(sizeof(a) / sizeof(a[0])),          \
								   (char **)(uintptr_t)a, &(d), e, sizeof(e)); \
	if (!ok) fprintf(stderr, "  parse error: %s\n", e);                      \
	CHECK(ok, "the command under test parses, so its result can be read");   \
} while (0)

static int overlay_flag_tests(void) {
	CHECK(g_overlay->ipc_verb != NULL, "overlay registers an IPC verb");
	CHECK(strcmp(g_overlay->ipc_verb, "overlay") == 0, "the verb is spelled overlay, which is a frozen user contract");
	CHECK(g_overlay->ipc_command != NULL, "overlay supplies a command handler");

	OL_OK("-t", "hello");
	OL_OK("--text", "hello");
	OL_OK("-i", "/tmp/a.png");        OL_OK("--image", "/tmp/a.png");
	OL_OK("-du", "3000");             OL_OK("--duration", "3000");
	OL_OK("-fi", "250");              OL_OK("--fade-in", "250");
	OL_OK("-fo", "250");              OL_OK("--fade-out", "250");
	OL_OK("-b", "1");                 OL_OK("--burn", "1");
	OL_OK("--art", "1");
	OL_OK("-aa", "0.5");              OL_OK("--art-alpha", "0.5");
	OL_OK("-as", "0.3");              OL_OK("--art-size", "0.3");
	OL_OK("-ap", "behind");           OL_OK("--art-pos", "left");
	OL_OK("-tc", "ff0000ff");         OL_OK("--text-color", "ff0000ff");
	OL_OK("-ts", "48");               OL_OK("--text-size", "48");
	OL_OK("-sc", "000000ff");         OL_OK("--stroke-color", "000000ff");
	OL_OK("-sw", "2");                OL_OK("--stroke-width", "2");
	OL_OK("-f", "serif");             OL_OK("--font", "serif");
	OL_OK("-x", "0.5");               OL_OK("-y", "100px");
	OL_OK("-w", "0.8");               OL_OK("--width", "0.8");
	OL_OK("-h", "0.2");               OL_OK("--height", "0.2");
	OL_OK("-a", "center");            OL_OK("--anchor", "top-left");
	OL_OK("-l", "back");              OL_OK("--layer", "front");

	OL_OK("-ap", "right");  OL_OK("-ap", "above");  OL_OK("-ap", "below");
	OL_OK("-a", "bottom-right");

	OL_OK("-t", "white rabbit");
	OL_OK("-t", "line one\nstill line one");
	OL_OK("-t", "a", "-t", "b", "-t", "c");

	OL_ERR("--bogus", "1");
	OL_ERR("-t");
	OL_ERR("--font");

	struct overlay_spec d;

	OL_PARSE(d, "-t", "one", "-t", "two", "-t", "three");
	CHECK(strcmp(d.text, "one\ntwo\nthree") == 0, "a repeated text flag joins its values with newlines");

	OL_PARSE(d, "-t", "solo");
	CHECK(strcmp(d.text, "solo") == 0, "a single text flag stands alone");

	OL_PARSE(d, "-t", "white rabbit");
	CHECK(strcmp(d.text, "white rabbit") == 0, "a space inside a text value survives");

	OL_PARSE(d, "-f", "DejaVu Sans");
	CHECK(strcmp(d.font, "DejaVu Sans") == 0, "a space survives inside a non-text value too");
	OL_PARSE(d, "-i", "/tmp/album art.png");
	CHECK(strcmp(d.image_path, "/tmp/album art.png") == 0, "a space survives inside a path");

	OL_PARSE(d, "-t", "a\nb", "-t", "c");
	CHECK(strcmp(d.text, "a\nb\nc") == 0, "a newline inside one text value stays inside it and eats nothing after");

	OL_PARSE(d, "-du", "3000", "-fi", "250", "-fo", "500", "-b", "1");
	CHECK(d.duration_ms == 3000, "duration reaches the parsed command");
	CHECK(d.fade_in_ms == 250, "fade-in reaches the parsed command");
	CHECK(d.fade_out_ms == 500, "fade-out reaches the parsed command");
	CHECK(d.burn == 1, "burn reaches the parsed command");

	OL_PARSE(d, "-l", "back");
	CHECK(d.layer == SCENE_LAYER_BACK, "the back layer name maps to the back layer");
	OL_PARSE(d, "-l", "front");
	CHECK(d.layer == SCENE_LAYER_FRONT, "the front layer name maps to the front layer");

	OL_PARSE(d, "-ap", "behind");
	CHECK(d.art_placement == OVERLAY_ART_BEHIND, "an art placement name maps to its placement");
	OL_PARSE(d, "-ap", "below");
	CHECK(d.art_placement == OVERLAY_ART_BELOW, "each art placement name maps to a distinct placement");

	OL_PARSE(d, "-tc", "ff0000ff");
	CHECK(strcmp(d.text_color, "ff0000ff") == 0, "a text color reaches the parsed command");

	OL_PARSE(d, "-t", "x");
	CHECK(d.duration_ms == OVERLAY_SPEC_DEFAULT, "an unnamed flag keeps its sentinel rather than a zero");
	CHECK(d.font[0] == '\0', "an unnamed string flag stays empty");

	return 0;
}

int main(void) {
	module_registry_visit(find_overlay, NULL);
	CHECK(g_overlay != NULL, "overlay registers a descriptor under prefix overlay");
	CHECK(g_overlay->config_defaults != NULL, "overlay supplies a defaults hook");
	CHECK(g_overlay->config_parse != NULL, "overlay supplies a parse hook");

	g_overlay->config_defaults();
	CHECK(overlay_burn_enabled() == 1, "burn defaults on, and the accessor reports it");

	CHECK(g_overlay->config_parse("font", "serif") == 1, "parse claims the font key");
	CHECK(g_overlay->config_parse("color", "ff0000ff") == 1, "parse claims the color key");
	CHECK(g_overlay->config_parse("w", "0.5") == 1, "parse claims the width key");
	CHECK(g_overlay->config_parse("anchor", "center") == 1, "parse claims the anchor key");
	CHECK(g_overlay->config_parse("layer", "1") == 1, "parse claims the layer key");
	CHECK(g_overlay->config_parse("info_duration", "5000") == 1, "parse claims the info_duration key");
	CHECK(g_overlay->config_parse("fade_in_ms", "250") == 1, "parse claims the fade_in_ms key");
	CHECK(g_overlay->config_parse("burn", "0") == 1, "parse claims the burn key");
	CHECK(g_overlay->config_parse("port", "9100") == 0, "parse rejects port, which another module owns");
	CHECK(g_overlay->config_parse("bogus", "x") == 0, "parse rejects an unknown key");

	CHECK(overlay_burn_enabled() == 0, "accessor reports the parsed burn value");

	/* Layer has no getter, so this only has to be survivable. The style it
	 * applies is covered by the daemon smoke. */
	overlay_clamp_layer_to_back("test");
	CHECK(overlay_burn_enabled() == 0, "clamping the layer leaves the rest of the slice alone");

	if (overlay_flag_tests() != 0) return 1;

	CHECK_PASS("test-overlay");
}
