// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 rakwahye
/** @file ipc.h
 * @brief Command IPC.
 *
 * Splits the verb from each line and dispatches to the module that
 * registered it. Lifecycle verbs (preset, reload, quit, help) are
 * handled inline. `help` enumerates the registered set live. */

#ifndef IPC_H
#define IPC_H

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_callbacks {
	int (*load_preset)(const char *path);
	void (*reload)(void);
	void (*quit)(void);
};

/** IPC config slice. key is "ipc.sock_path", the unix-socket path that
 * the IPC listener binds. */
struct ipc_config {
	char sock_path[256];
};

struct rt;
int ipc_start(const struct ipc_callbacks *cb, struct rt *rt);
void ipc_stop(void);

#ifdef __cplusplus
}
#endif

#endif
