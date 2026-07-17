// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file cli.c
 * @brief Option table assembly and parse.
 *
 * Flattens every registered group into one getopt_long table,
 * then dispatches each option to its handler or stashes its sticky
 * override. Overrides apply through an injected sink to avoid a
 * config dependency. */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>

#include "cli.h"
#include "module_registry.h"
#include "app_paths.h"

#define CLI_MAX_OPTIONS 64

/* Flattened registered-option table. Each slot points at a caller-owned (static)
 * cli_option plus the per-run parsed state for sticky overrides. */
struct cli_slot {
	const char *group;
	const struct cli_option *opt;
	int val; // getopt_long return value for this option
	bool ov_set; // sticky override: was a value parsed?
	char ov_val[32];
};

static struct cli_slot s_slots[CLI_MAX_OPTIONS];
static int s_n_slots = 0;
static int s_next_synth_val = 256; // long-only option vals

static void (*s_apply_kv)(const char *key, const char *val) = NULL;

void cli_set_override_sink(void (*apply_kv)(const char *key, const char *val))
{
	s_apply_kv = apply_kv;
}

int cli_register(const char *group, const struct cli_option *opts, int n)
{
	for (int i = 0; i < n; i++) {
		if (s_n_slots >= CLI_MAX_OPTIONS) {
			fprintf(stderr, "[cli] too many options (max %d)\n", CLI_MAX_OPTIONS);
			return -1;
		}
		struct cli_slot *s = &s_slots[s_n_slots++];
		s->group = group;
		s->opt = &opts[i];
		s->ov_set = false;
		s->ov_val[0] = '\0';
		/* A short option uses its character as the getopt val; long-only opts
		 * get a synthetic val >= 256 (outside the char range). */
		s->val = opts[i].short_name ? opts[i].short_name : s_next_synth_val++;
	}
	return 0;
}

static const char *prog_basename(const char *argv0)
{
	const char *base = argv0 ? strrchr(argv0, '/') : NULL;
	return base ? base + 1 : (argv0 ? argv0 : app_paths_app_name());
}

static void print_usage(FILE *out, const char *prog)
{
	fprintf(out, "usage: %s [options]\n", prog);
	const char *cur_group = NULL;
	for (int i = 0; i < s_n_slots; i++) {
		const struct cli_option *o = s_slots[i].opt;
		if (s_slots[i].group != cur_group) {
			cur_group = s_slots[i].group;
			fprintf(out, "\n %s:\n", cur_group ? cur_group : "options");
		}
		char names[48];
		if (o->short_name)
			snprintf(names, sizeof(names), "-%c, --%s", o->short_name, o->long_name);
		else
			snprintf(names, sizeof(names), "    --%s", o->long_name);
		fprintf(out, "  %-24s %s\n", names, o->help ? o->help : "");
	}
	fprintf(out, "\n  -h, --help               show this help and exit\n");
}

/* Registry callback: invoke each module's register_cli() hook. */
static void cli_cb_register(const struct module_descriptor *d, void *ud)
{
	(void)ud;
	if (d->register_cli) d->register_cli();
}

bool cli_parse(int argc, char **argv)
{
	/* 1. Collect option groups: walk the registry (modules) ONCE, on top of
	 *    any core group main registered before calling us. Idempotent so a
	 *    re-parse (or a test calling twice) doesn't duplicate module options. */
	static bool s_walked = false;
	if (!s_walked) {
		module_registry_visit(cli_cb_register, NULL);
		s_walked = true;
	}

	const char *prog = prog_basename(argv[0]);

	/* 2. Build getopt_long inputs from the flattened table. */
	struct option longopts[CLI_MAX_OPTIONS + 2];
	char optstring[2 * CLI_MAX_OPTIONS + 4];
	int no = 0, oi = 0;
	optstring[oi++] = ':'; // leading ':' -> distinguish missing-arg
	for (int i = 0; i < s_n_slots; i++) {
		const struct cli_option *o = s_slots[i].opt;
		longopts[no].name = o->long_name;
		longopts[no].has_arg = (o->has_arg == CLI_REQUIRED_ARG) ? required_argument : no_argument;
		longopts[no].flag = NULL;
		longopts[no].val = s_slots[i].val;
		no++;
		if (o->short_name) {
			optstring[oi++] = (char)o->short_name;
			if (o->has_arg == CLI_REQUIRED_ARG) optstring[oi++] = ':';
		}
	}
	/* built-in --help/-h */
	longopts[no].name = "help"; longopts[no].has_arg = no_argument;
	longopts[no].flag = NULL; longopts[no].val = 'h';
	no++;
	optstring[oi++] = 'h';
	longopts[no] = (struct option){ 0, 0, 0, 0 };
	optstring[oi] = '\0';

	/* 3. Parse + dispatch. */
	optind = 1;
	int c;
	while ((c = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
		if (c == 'h') { print_usage(stdout, prog); exit(0); }
		if (c == ':') {
			fprintf(stderr, "%s: missing argument\n", prog);
			print_usage(stderr, prog);
			return false;
		}
		if (c == '?') { print_usage(stderr, prog); return false; }

		/* Find the slot whose val matches. */
		struct cli_slot *slot = NULL;
		for (int i = 0; i < s_n_slots; i++)
			if (s_slots[i].val == c) { slot = &s_slots[i]; break; }
		if (!slot) { print_usage(stderr, prog); return false; }

		const struct cli_option *o = slot->opt;
		if (o->handler) {
			if (o->handler(optarg) != 0) return false; // handler reported
		} else if (o->config_key) {
			char *endp = NULL;
			long v = strtol(optarg, &endp, 10);
			if (!endp || *endp || v < o->lo || v > o->hi) {
				fprintf(stderr,
				    "error: --%s requires an integer in [%ld..%ld] (got '%s')\n",
				    o->long_name, o->lo, o->hi, optarg ? optarg : "");
				return false;
			}
			snprintf(slot->ov_val, sizeof(slot->ov_val), "%ld", v);
			slot->ov_set = true;
		}
		/* else: option with neither handler nor config_key - ignored (no-op). */
	}
	return true;
}

void cli_apply_overrides(bool announce)
{
	if (!s_apply_kv) return;
	for (int i = 0; i < s_n_slots; i++) {
		if (s_slots[i].ov_set && s_slots[i].opt->config_key) {
			if (announce)
				fprintf(stderr, "[cli] override %s = %s\n",
				        s_slots[i].opt->config_key, s_slots[i].ov_val);
			s_apply_kv(s_slots[i].opt->config_key, s_slots[i].ov_val);
		}
	}
}
