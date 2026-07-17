// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file cli.h
 * @brief Additive CLI option registration.
 *
 * Each module registers its own option group from its `register_cli`
 * hook, and the parser assembles getopt_long over the union. No central
 * option table. Dropping a module drops its options. */

#ifndef CLI_H
#define CLI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum cli_arg { CLI_NO_ARG = 0, CLI_REQUIRED_ARG = 1 };

struct cli_option {
    const char *long_name; // required, no leading dashes
    int short_name; // e.g. 'd'; 0 = long-only
    enum cli_arg has_arg;
    const char *help; // one-line help, shown by --help

    /** Direct handler. Mutually exclusive with `config_key`. */
    int (*handler)(const char *optarg);

    /** Sticky config-key override: set this (and `lo`/`hi`) instead of a
     * handler. Value validated as an int in `[lo,hi]`. */
    const char *config_key;
    long lo, hi;
};

/** Register an option group, typically from a module's `register_cli`
 * hook. `opts` must have static lifetime - the pointer is kept. `group`
 * labels the --help section.
 * @returns 0 on success, nonzero on overflow. */
int cli_register(const char *group, const struct cli_option *opts, int n);

/** Invoke every module's `register_cli`, then parse argv over the union
 * of all registered options. Owns -h/--help. Call `cli_register` for
 * any non-module (core) groups before this.
 * @returns true on success. false on a parse error or after printing
 * usage, on which the caller should exit nonzero. */
bool cli_parse(int argc, char **argv);

/** Re-apply every registered sticky config-key override by routing
 * `<config_key> <value>` through the config layer. CLI beats file.
 * `announce` true logs each applied override, false is silent. No-op
 * for handler options. */
void cli_apply_overrides(bool announce);

/** Install the sink `cli_apply_overrides` routes overrides through. */
void cli_set_override_sink(void (*apply_kv)(const char *key, const char *val));

#ifdef __cplusplus
}
#endif

#endif
