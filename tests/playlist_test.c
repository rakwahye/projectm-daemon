#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "playlist.h"
#include "module_registry.h"
#include "overlay.h"
#include "info_views.h"

#include "check.h"

/* Peers the module reaches, stubbed and never called. Returning NULL for the
 * active visualizer makes the load path take its safe no-engine early return,
 * so nothing here dereferences the vtable. Engine arbitration is covered by
 * the visualizer selection test. */
int g_debug = 0;
struct visualizer *visualizer_active(void) { return NULL; }
struct visualizer *visualizer_ensure_for(const char *path) { (void)path; return NULL; }
void info_preset(const char *text) { (void)text; }
void overlay_show(const struct overlay_spec *d) { (void)d; }

static const struct module_descriptor *g_pl = NULL;

static void find_pl(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_prefix && strcmp(d->config_prefix, "playlist") == 0) g_pl = d;
}

int main(void) {
	module_registry_visit(find_pl, NULL);
	CHECK(g_pl != NULL, "playlist registers a descriptor under prefix playlist");
	CHECK(g_pl->config_defaults != NULL, "playlist supplies a defaults hook");
	CHECK(g_pl->config_parse != NULL, "playlist supplies a parse hook");

	g_pl->config_defaults();
	playlist_config_apply();
	CHECK(playlist_is_shuffled() == 1, "shuffle defaults on, and the default reaches live state");

	CHECK(g_pl->config_parse("shuffle", "0") == 1, "parse claims the shuffle key");
	playlist_config_apply();
	CHECK(playlist_is_shuffled() == 0, "a parsed shuffle value reaches live state on apply");
	CHECK(g_pl->config_parse("auto_load", "jams.lst") == 1, "parse claims the auto_load key");

	CHECK(g_pl->config_parse("dir", "/x") == 0, "parse rejects dir, which the presets slice owns");
	CHECK(g_pl->config_parse("bogus", "x") == 0, "parse rejects an unknown key");

	{
		char dir[] = "/tmp/pltestXXXXXX";
		CHECK(mkdtemp(dir) != NULL, "a temp directory is available for the file-backed checks");

		/* Real readable files, because loading checks each with access(). The
		 * name header and the blacklist section frame them in the list file. */
		char pth[8][512], lst[600], out[600];
		for (int i = 0; i < 8; i++) {
			snprintf(pth[i], sizeof pth[i], "%s/p%d.milk", dir, i);
			FILE *pf = fopen(pth[i], "w");
			CHECK(pf != NULL, "each backing preset file is creatable");
			fputs("x\n", pf);
			fclose(pf);
		}
		snprintf(lst, sizeof lst, "%s/list.lst", dir);
		FILE *lf = fopen(lst, "w");
		CHECK(lf != NULL, "the list file is creatable");
		fputs("#name My Favorites\n", lf);
		for (int i = 0; i < 8; i++) fprintf(lf, "%s\n", pth[i]);
		fprintf(lf, "#blacklist\n%s/p99.milk\n", dir);
		fclose(lf);

		CHECK(playlist_load_file(lst) == 1, "a list file loads");
		CHECK(playlist_count() == 8, "entries below the blacklist marker are not counted");
		for (int i = 0; i < 8; i++)
			CHECK(strcmp(playlist_at(i), pth[i]) == 0, "shuffle is non-destructive, so memory order mirrors file order");
		CHECK(playlist_idx() == 0, "an unshuffled load starts at the first entry");

		playlist_set_shuffled(1);
		srand(12345);
		int visited[5];
		visited[0] = playlist_idx();
		for (int k = 1; k < 5; k++) {
			playlist_next();
			visited[k] = playlist_idx();
			CHECK(visited[k] >= 0 && visited[k] < 8, "a shuffled next lands inside the list");
		}
		for (int k = 4; k > 0; k--) {
			playlist_prev();
			CHECK(playlist_idx() == visited[k - 1], "prev walks back the entries actually visited, in reverse");
		}

		int here = playlist_idx();
		playlist_prev();
		CHECK(playlist_idx() == here, "prev is a no-op once the visit history is exhausted");

		playlist_set_shuffled(0);
		playlist_set_idx(3);
		playlist_prev();
		CHECK(playlist_idx() == 2, "unshuffled prev steps back one index");
		playlist_next();
		CHECK(playlist_idx() == 3, "unshuffled next steps forward one index");

		snprintf(out, sizeof out, "%s/out.lst", dir);
		CHECK(playlist_save_file(out) == 1, "the list saves");
		FILE *of = fopen(out, "r");
		CHECK(of != NULL, "the saved list is readable back");
		char l0[600], l1[600];
		CHECK(fgets(l0, sizeof l0, of) != NULL, "the saved list has a first line");
		CHECK(strncmp(l0, "#name My Favorites", 18) == 0, "save round-trips the name header verbatim");
		CHECK(fgets(l1, sizeof l1, of) != NULL, "the saved list has an entry after the header");
		l1[strcspn(l1, "\n")] = '\0';
		CHECK(strcmp(l1, pth[0]) == 0, "save keeps file order, because nothing ever reordered it");
		fclose(of);

		CHECK(playlist_load_file(out) == 1, "reloading the saved list clears the visit history");

		unlink(lst); unlink(out);
		for (int i = 0; i < 8; i++) unlink(pth[i]);
		rmdir(dir);
	}

	CHECK_PASS("test-playlist");
}
