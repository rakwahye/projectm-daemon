// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file module_registry.c
 * @brief Walks over the registered modules.
 *
 * Iterates the `visualizer_modules` section forward and reverse for
 * init, shutdown, and lookups. A sentinel entry with an empty name
 * is emitted here and skipped by every walk, so an otherwise-empty
 * section still has valid bounds. */

#include "module_registry.h"
#include "util.h"
#include <stddef.h>

/* Sentinel descriptor. Empty so walker skips it. */
static const struct module_descriptor s_sentinel_desc = {
	.name = "",
};

static const struct module_descriptor *const s_sentinel_ptr
	__attribute__((used, section("visualizer_modules"))) =
		&s_sentinel_desc;

void module_registry_visit(
	void (*fn)(const struct module_descriptor *desc, void *ud),
	void *ud)
{
	if (!fn) return;
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		fn(d, ud);
	}
}

void module_registry_visit_reverse(void (*fn)(const struct module_descriptor *desc, void *ud),
                                   void *ud)
{
	if (!fn) return;
	/* Decrement at top of body. Never form a pointer below __start. */
	for (const struct module_descriptor *const *p = __stop_visualizer_modules;
	     p > __start_visualizer_modules; ) {
		p--;
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		fn(d, ud);
	}
}

const struct module_descriptor *module_registry_init_all(struct rt *rt)
{
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		if (d->init && !d->init(rt)) return d;
	}
	return NULL;
}

size_t module_registry_count(void)
{
	size_t n = 0;
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		n++;
	}
	return n;
}

const struct frame_pacer *module_active_pacer(void)
{
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		if (d->pacer) return d->pacer;
	}
	return NULL;
}

const struct module_descriptor *module_engine_for(const char *name)
{
	const struct module_descriptor *fallback = NULL;
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		if (!d->create) continue; // not an engine
		if (d->file_extensions && d->file_extensions[0]) {
			if (suffix_in_list(name, d->file_extensions))
				return d; // claims this suffix
		} else if (!fallback) {
			fallback = d; // no-ext = fallback
		}
	}
	return fallback;
}

const struct module_descriptor *module_engine_find(
	struct visualizer *(*fn)(void))
{
	if (!fn) return NULL;
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		if (d->create == fn) return d;
	}
	return NULL;
}

/* Message sink lookup. */
static void (*active_message_sink(void))(const char *source, const char *reply)
{
	for (const struct module_descriptor *const *p = __start_visualizer_modules;
		 p < __stop_visualizer_modules; p++) {
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		if (d->message_sink) return d->message_sink;
	}
	return NULL;
}

void module_emit_reply(const char *source, const char *reply)
{
	void (*sink)(const char *, const char *) = active_message_sink();
	if (sink && reply && reply[0]) sink(source, reply);
}

void module_registry_unwind(const struct module_descriptor *failed)
{
	int past_failed = (failed == NULL);
	for (const struct module_descriptor *const *p = __stop_visualizer_modules;
		 p > __start_visualizer_modules; ) {
		p--;
		const struct module_descriptor *d = *p;
		if (!d || !d->name || d->name[0] == '\0') continue;
		if (!past_failed) {
			if (d == failed) past_failed = 1;
			continue;
		}
		if (d->shutdown) d->shutdown();
	}
}
