// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file config.h
 * @brief Config router.
 *
 * Loads key=value config and routes each `<module>.<subkey>` to the
 * owning module's slice parser. Owns no schema of its own. A missing
 * file is fine: one is auto-created from each module's defaults. */

#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/** Load config from default XDG path. Auto-creates if missing. */
void config_load(void);

/** Load config from a specific path. */
void config_load_from(const char *path);

/** Apply one key=value to live config, routing it to the owning
 * module's slice parser (by "<prefix>.<subkey>"). Every config source
 * feeds this. Unknown keys warn and are ignored. */
void config_apply_kv(const char *key, const char *val);

/** Re-apply every module's parsed slice to its live state, by walking
 * the registry's config_apply hooks. config_load re-parses the slices,
 * this stages the result. */
void config_apply_all(void);

/** Get the default config file path (for -c fallback). */
void config_default_path(char *buf, int buflen);

/** Set a single key in the config file (in-place update or prepend).
 * Atomic via tempfile+rename. Returns 1 on success. */
int config_set_key(const char *path, const char *key, const char *value);

/** Path of the config file config_load(_from) last loaded. Used for
 * in-place key setting. */
const char *config_active_path(void);

#ifdef __cplusplus
}
#endif

#endif
