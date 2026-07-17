// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file pending.h
 * @brief Render-thread executor for the pending mailbox.
 *
 * Drains one queued action per frame from the render prologue and
 * routes it to the owning module. The consumer side of the runtime's
 * post calls. */

#ifndef PENDING_H
#define PENDING_H

#ifdef __cplusplus
extern "C" {
#endif

struct rt;
void apply_pending(struct rt *rt);

#ifdef __cplusplus
}
#endif

#endif
