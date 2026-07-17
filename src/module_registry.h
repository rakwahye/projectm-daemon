// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file module_registry.h
 * @brief Module self-registration.
 *
 * Each module emits one descriptor into the `visualizer_modules`
 * linker section via `MODULE_REGISTER`. A walker iterates the
 * section at startup, so a module opts in from its own source file
 * and nothing central lists them. Dropping the source file drops
 * the registration. */

#ifndef MODULE_REGISTRY_H
#define MODULE_REGISTRY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Render-pipeline phase slots. A render module names the slot it
 * draws in. The compositor dispatches the slots in listed order. */
enum {
	RENDER_PHASE_EFFECT = 1, // full-frame pass over the composed scene
	RENDER_PHASE_PRESENT = 2, // module draws its own content on top
};

struct ipc_command_ctx {
	int fd; // Live client socket
	const char *args; // after the verb.
	int argc;
	char **argv; // words byte-exact.
	const char *body; // payload after the command frame
	int body_len;
	int output_w; // current dims, 0 = unconfigured. for coord resolution.
	int output_h;
	char *reply; // handler writes reply text here
	int reply_len;
};

/** `ipc_command` return: decline the line so IPC keeps walking. */
#define IPC_CMD_DECLINED (-1)

struct rt;
struct visualizer;

/** Frame-pacing vtable. A pacing module registers via the descriptor's
 * `pacer` field, reached through `module_active_pacer`, which calls
 * only the hooks it needs. Compiled out, `module_active_pacer` returns
 * NULL and every call site no-ops. */
struct frame_pacer {
	void (*init)(int vrefresh_hz, const void *pace_ctx);
	void (*reload)(void);
	void (*repush)(void);
	void (*summary)(void);
	void (*poll)(void); // frame start: harvest timers
	int (*should_skip)(void); // return nonzero to skip rendering
	void (*observe)(int transitioning); // decision pass
	void (*presented)(void); // after present
	void (*measure_begin)(void);
	void (*measure_end)(void);
	void (*preset_cut)(void);
	int (*fps_ceiling)(void); // fps cap, 0 = none
};

struct module_descriptor {
	/** Doubles as a "registered" marker. Sentinel uses "". */
	const char *name;

	/** Shared runtime handle. A module may ignore it if it only needs its
	 * own config slice. */
	int (*init)(struct rt *rt);
	void (*shutdown)(void);

	/** Config slice. Modules own their slice as file-static storage. These
	 * hooks close over it, so no slice pointer is passed and config never
	 * learns the slice's type. */
	const char *config_prefix; // NULL = No config slice
	void (*config_defaults)(void); // Resets default. NULL = No config slice
	/** Apply one key under the prefix. @returns 1 if the subkey was recognized. */
	int (*config_parse)(const char *subkey, const char *val);
	void (*config_apply)(void); // NULL = read live
	const char *file_extensions;

	struct visualizer *(*create)(void);

	/** Default config lines. "<prefix>.<key>=<val>  # note" per line. */
	const char *config_template;

	int (*register_cli)(void); // returns 0 on success. NULL = no CLI options

	/** Render pipeline: a render module draws into the scene each frame.
	 * `render` runs at `render_phase` (RENDER_PHASE_*). `render_init` and
	 * `render_destroy` bracket its GL resources (called with EGL current).
	 * `render` NULL means the module draws nothing. */
	int render_phase;
	void (*render_init)(void);
	void (*render)(int width, int height);
	void (*render_destroy)(void);

	/** IPC command: the verb(s) this module owns plus its handler.
	 *
	 * Single-verb: set `ipc_verb`. IPC prefix-matches it and calls
	 * `ipc_command` with args past the verb. Return 0 to have IPC close
	 * the fd, 1 to take fd ownership.
	 *
	 * Line-handler (`ipc_verb` NULL): IPC calls `ipc_command` with the
	 * whole line for every otherwise-unhandled command. The handler
	 * returns IPC_CMD_DECLINED to pass (IPC keeps walking), or 0/1 as
	 * above. For a verb cluster that doesn't fit a single prefix. */
	const char *ipc_verb;
	int (*ipc_command)(struct ipc_command_ctx *c);
	const char *ipc_help;

	/** Frame pacing. A non-NULL pacer makes this module the frame pacer,
	 * reached via `module_active_pacer`. NULL = not a pacer. */
	const struct frame_pacer *pacer;

	/** On-screen message sink. The module owning the overlay fills this. It
	 * receives the source module name and the renderer's raw reply line, and
	 * owns all presentation: prefix strip, gating, bounding. NULL = no sink.
	 * Messages are dropped. */
	void (*message_sink)(const char *source, const char *reply);
};

/**
 * MODULE_REGISTER(modname, .field = value, ...)
 *
 * Declares a static descriptor and a pointer-to-descriptor in the
 * visualizer_modules section. Use exactly once per module, in that
 * module's source file. Fields not named default to NULL (per C
 * designated-initializer semantics).
 *
 * Why an indirect pointer: when each translation unit emits a struct
 * directly into the section, the linker aligns each TU's contribution
 * to the strictest section alignment (often 32 bytes) and pads the gap.
 * The walker's `++` then trips over padding bytes interpreted as garbage
 * descriptors. Storing a pointer instead means each TU contributes
 * exactly sizeof(void *) bytes, which matches the pointer's natural
 * alignment - no padding, dense iteration.
 *
 * @code
 * static struct demo_config s_cfg;
 * static void demo_config_defaults(void) { s_cfg.alpha = 0.0f; ... }
 * static int  demo_config_parse(const char *k, const char *v) {
 *   if (!strcmp(k, "color")) { ...; return 1; }
 *   return 0;
 * }
 * MODULE_REGISTER(demo,
 *   .config_prefix   = "demo",
 *   .config_defaults = demo_config_defaults,
 *   .config_parse    = demo_config_parse);
 * @endcode
 */
#define MODULE_REGISTER(modname, ...) \
	static const struct module_descriptor _module_##modname##_desc = { \
	    .name = #modname, \
	    __VA_ARGS__ \
	}; \
	static const struct module_descriptor *const _module_##modname##_ptr \
	    __attribute__((used, section("visualizer_modules"))) = \
	        &_module_##modname##_desc

/** Linker-provided section bounds. Each entry is a pointer to a
 * module_descriptor. */
extern const struct module_descriptor *const __start_visualizer_modules[];
extern const struct module_descriptor *const __stop_visualizer_modules[];

/** Visit every registered module in section order, skipping the sentinel.
 * `ud` is forwarded straight through to the callback for caller state. */
void module_registry_visit(
	void (*fn)(const struct module_descriptor *desc, void *ud),
	void *ud);

/** Same as `module_registry_visit` but in reverse section order. Skips
 * the sentinel. `ud` is forwarded through. */
void module_registry_visit_reverse(
	void (*fn)(const struct module_descriptor *desc, void *ud),
	void *ud);

/** Init-pass walk. Runs each module's `init(rt)`, stopping at the first
 * that returns 0 (failure) and returning that descriptor so the caller
 * can name it. @returns NULL when all succeeded. */
const struct module_descriptor *module_registry_init_all(struct rt *rt);

/** Count of registered modules. Sentinel excluded. */
size_t module_registry_count(void);

/** The active frame pacer: the first registered module with a non-NULL
 * pacer. @returns NULL if none is compiled in. */
const struct frame_pacer *module_active_pacer(void);

/** The engine module that handles `name`, chosen by file suffix. Returns
 * the first create-capable descriptor whose `file_extensions` claims the
 * suffix, else the first create-capable descriptor advertising none. */
const struct module_descriptor *module_engine_for(const char *name);

/** The descriptor whose `create` is `fn`. Lets startup label the
 * factory-created initial engine with its own descriptor, so the first
 * load is a same-engine no-op rather than a needless teardown.
 * @returns NULL if no descriptor matches. */
const struct module_descriptor *module_engine_find(
	struct visualizer *(*fn)(void));

/** Forward a renderer's reply to the active message sink. No-op when no
 * sink is registered. */
void module_emit_reply(const char *source, const char *reply);

/** Failure-path shutdown, opposite to init order. `failed` NULL shuts
 * down every module. Otherwise shuts down only those that initialized
 * strictly before `failed`, skipping `failed` and the uninitialized
 * tail. */
void module_registry_unwind(const struct module_descriptor *failed);

#ifdef __cplusplus
}
#endif

#endif
