// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file util.c
 * @brief Small shared helpers. */

#include "util.h"
#include <string.h>
#include <ctype.h>

void path_strip_trailing_slashes(char *s) {
	if (!s) return;
	int n = (int)strlen(s);
	while (n > 1 && s[n - 1] == '/')
		s[--n] = '\0';
}

static int ci_suffix(const char *name, size_t nlen,
                     const char *suf, size_t slen) {
	if (!slen || slen > nlen) return 0;
	const char *a = name + nlen - slen;
	for (size_t i = 0; i < slen; i++)
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)suf[i]))
			return 0;
	return 1;
}

int suffix_in_list(const char *name, const char *ext_list) {
	if (!name || !ext_list || !ext_list[0]) return 0;
	size_t nlen = strlen(name);
	const char *p = ext_list;
	while (*p) {
		while (*p == ' ') p++;
		const char *start = p;
		while (*p && *p != ' ') p++;
		if (ci_suffix(name, nlen, start, (size_t)(p - start)))
			return 1;
	}
	return 0;
}
