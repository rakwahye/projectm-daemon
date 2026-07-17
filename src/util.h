// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file util.h
 * @brief Small shared helpers. */

#ifndef UTIL_H
#define UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/** Strip trailing '/'. Keep lone '/'. Safe on NULL. */
void path_strip_trailing_slashes(char *s);

/** Case-insensitive suffix test against list.
 * @returns 1 = found */
int suffix_in_list(const char *name, const char *ext_list);

#ifdef __cplusplus
}
#endif

#endif
