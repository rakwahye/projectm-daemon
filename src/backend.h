// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file backend.h
 * @brief Display backend selection.
 *
 * Holds the configured backend mode. The wiring reads it to choose
 * which output backend to bring up. */

#ifndef BACKEND_H
#define BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

struct backend_config {
	char mode[16];
};

const char *backend_mode(void);

#ifdef __cplusplus
}
#endif

#endif
