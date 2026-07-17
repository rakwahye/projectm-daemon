// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file config.c
 * @brief Config file parse and key routing.
 *
 * One key=value per line. `#` starts a comment, `\#` is a literal
 * hash, blank lines are skipped. Each key routes to its module
 * slice. Unknown keys warn. Writes back a single key atomically via
 * tempfile+rename. */

#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "app_paths.h"
#include "module_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

static void ensure_dir(const char *path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static char s_active_config_path[512];

const char *config_active_path(void) { return s_active_config_path; }

static char *strip(char *s) {
	while (*s && isspace((unsigned char)*s)) s++;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) end--;
	*end = '\0';
	return s;
}

/* Cut a value at the first UNESCAPED '#', then trim the whitespace left
 * before the cut. A preceding backslash escapes a hash. "\#" collapses
 * to a literal '#' and does not start a comment. */
static void strip_inline_comment(char *s) {
	char *w = s, *r = s;
	while (*r) {
		if (*r == '\\' && r[1] == '#') { *w++ = '#'; r += 2; continue; }
		if (*r == '#') break;
		*w++ = *r++;
	}
	*w = '\0';
	while (w > s && isspace((unsigned char)w[-1])) *--w = '\0';
}

/* Every config source feeds this one router (route_line / config_apply_kv),
 * so a reload is just: re-run every slice's defaults, then re-feed. */

static void cfg_cb_defaults(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_defaults) d->config_defaults();
}

struct cfg_route_ctx {
	const char *prefix;
	const char *subkey;
	const char *val;
	int found; // registered module owns this prefix
	int handled; // module parser recognized the subkey
};

static void cfg_cb_route(const struct module_descriptor *d, void *ud) {
	struct cfg_route_ctx *c = ud;
	if (c->found) return; // first owner wins
	if (!d->config_prefix || !d->config_parse) return;
	if (strcmp(d->config_prefix, c->prefix) != 0) return;
	c->found = 1;
	c->handled = d->config_parse(c->subkey, c->val);
}

/* Route one stripped keyval pair. `key` is a writable buffer. A dotted
 * key goes to its owning module. */
static void route_line(char *key, const char *val) {
	char *dot = strchr(key, '.');
	if (dot) {
		*dot = '\0';
		struct cfg_route_ctx c = {
			.prefix = key, .subkey = dot + 1, .val = val,
		};
		module_registry_visit(cfg_cb_route, &c);
		*dot = '.'; // restore for the warn path
		if (c.found) {
			if (!c.handled)
				fprintf(stderr, "[config] unknown key ignored: %s\n", key);
			return;
		}
	}
	fprintf(stderr, "[config] unknown key ignored: %s\n", key);
}

static void apply_all_defaults(void) {
	module_registry_visit(cfg_cb_defaults, NULL);
}

/* Reload-path walk. Tell each module to re-apply its parsed slice to
 * live state. Modules that read their slice live leave `config_apply`
 * NULL. */
static void cfg_cb_apply(const struct module_descriptor *d, void *ud) {
	(void)ud;
	if (d->config_apply) d->config_apply();
}

void config_apply_all(void) {
	module_registry_visit(cfg_cb_apply, NULL);
}

/* Copies the key so callers can pass a const literal (route_line writes to it). */
void config_apply_kv(const char *key, const char *val) {
	char kbuf[256];
	snprintf(kbuf, sizeof(kbuf), "%s", key);
	route_line(kbuf, val);
}

static void load_file(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return;

	fprintf(stderr, "[config] loading %s\n", path);

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char *s = strip(line);
		if (!*s || *s == '#') continue;

		char *eq = strchr(s, '=');
		if (!eq) continue;

		*eq = '\0';
		char *key = strip(s);
		char *val = strip(eq + 1);
		strip_inline_comment(val);
		route_line(key, val);
	}
	fclose(f);
}

/* Emit one module's default-config block. ud is the open FILE*. Each
 * module advertises its own lines via the descriptor's config_template. */
static void cfg_cb_template(const struct module_descriptor *d, void *ud) {
	FILE *f = ud;
	if (d->config_template) fputs(d->config_template, f);
}

static void create_default_config(const char *dir, const char *path) {
	ensure_dir(dir);

	/* Don't overwrite existing */
	FILE *f = fopen(path, "r");
	if (f) { fclose(f); return; }

	f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "[config] cannot create %s: %s\n", path, strerror(errno));
		return;
	}

	module_registry_visit(cfg_cb_template, f);

	fclose(f);
	fprintf(stderr, "[config] created default config at %s\n", path);
}

void config_default_path(char *buf, int buflen) {
	snprintf(buf, buflen, "%s", app_paths_config_file());
}

void config_load(void) {
	apply_all_defaults();

	char path[512];
	config_default_path(path, sizeof(path));

	create_default_config(app_paths_config_dir(), path);

	char pldir[512];
	snprintf(pldir, sizeof(pldir), "%s", app_paths_playlists_dir());
	ensure_dir(pldir);

	load_file(path);
	snprintf(s_active_config_path, sizeof(s_active_config_path), "%s", path);
}

void config_load_from(const char *path) {
	apply_all_defaults();
	load_file(path);
	snprintf(s_active_config_path, sizeof(s_active_config_path), "%s", path);
}

/* Read the whole file into a NUL-terminated heap buffer. Configs are a few KB,
 * so one doubling allocation covers it. Sets `*out_len` to the byte count, not
 * counting the terminator. Returns NULL if the file cannot be read. */
static char *config_slurp(const char *path, size_t *out_len) {
	FILE *in = fopen(path, "r");
	if (!in) return NULL;

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		fclose(in);
		return NULL;
	}

	for (;;) {
		if (len + 1024 > cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				fclose(in);
				return NULL;
			}
			buf = grown;
		}
		size_t r = fread(buf + len, 1, cap - len - 1, in);
		len += r;
		if (r == 0) break;
	}
	fclose(in);

	buf[len] = '\0';
	*out_len = len;
	return buf;
}

/* Locate the line assigning `key`: optional leading whitespace, the key,
 * optional whitespace, then '='. The key has to start its own line, so a
 * substring elsewhere never matches. Sets `*start` to the line's first byte and
 * `*end` just past its newline. Returns 0 if the key is assigned nowhere. */
static int config_find_key_line(char *buf, size_t len, const char *key,
                                char **start, char **end)
{
	size_t keylen = strlen(key);
	char *limit = buf + len;
	char *scan = buf;

	while (scan < limit) {
		char *p = scan;
		while (p < limit && (*p == ' ' || *p == '\t')) p++;

		if ((size_t)(limit - p) >= keylen && memcmp(p, key, keylen) == 0) {
			char *q = p + keylen;
			while (q < limit && (*q == ' ' || *q == '\t')) q++;
			if (q < limit && *q == '=') {
				char *eol = memchr(scan, '\n', (size_t)(limit - scan));
				*start = scan;
				*end = eol ? eol + 1 : limit;
				return 1;
			}
		}

		char *eol = memchr(scan, '\n', (size_t)(limit - scan));
		if (!eol) break;
		scan = eol + 1;
	}

	return 0;
}

/* Rewrite `path` with `line` swapped in for the byte range [start, end), or
 * prepended when `start` is NULL. Tempfile plus rename, so a reader never sees
 * a half-written config and a failure leaves the original intact. Returns 1 on
 * success. */
static int config_rewrite(const char *path, const char *buf, size_t len,
                          const char *start, const char *end,
                          const char *line, int line_len)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	FILE *out = fopen(tmp, "w");
	if (!out) return 0;

	int ok = 1;
	if (start) {
		size_t prefix_n = (size_t)(start - buf);
		size_t suffix_n = (size_t)((buf + len) - end);
		if (fwrite(buf, 1, prefix_n, out) != prefix_n) ok = 0;
		if (ok && fwrite(line, 1, line_len, out) != (size_t)line_len) ok = 0;
		if (ok && fwrite(end, 1, suffix_n, out) != suffix_n) ok = 0;
	} else {
		if (fwrite(line, 1, line_len, out) != (size_t)line_len) ok = 0;
		if (ok && fwrite(buf, 1, len, out) != len) ok = 0;
	}

	if (ok && fflush(out) != 0) ok = 0;
	if (ok && fsync(fileno(out)) != 0) ok = 0;
	fclose(out);

	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return 0;
	}
	return 1;
}

int config_set_key(const char *path, const char *key, const char *value) {
	/* A missing file is not an error: the caller may be running against a
	 * custom config that was never created. */
	size_t len = 0;
	char *buf = config_slurp(path, &len);
	if (!buf) return 0;

	char line[1024];
	int line_len = snprintf(line, sizeof(line), "%s=%s\n", key, value);
	if (line_len < 0 || line_len >= (int)sizeof(line)) {
		free(buf);
		return 0;
	}

	char *start = NULL, *end = NULL;
	if (!config_find_key_line(buf, len, key, &start, &end))
		start = end = NULL; // absent, so the line goes on top

	int ok = config_rewrite(path, buf, len, start, end, line, line_len);
	free(buf);
	return ok;
}
