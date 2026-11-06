/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KV_H
#define KV_H

#include <gio/gio.h>
#include <glib.h>

#include "plugins/libkvp/kvp_msg.h"

#define KIVIEW_VERSION "0.0.3"

/* ---- kv_plugin.c: manifests and running plugins ---- */

typedef struct {
    char *name;
    char *exec;
    char **mimes;
    char **exts;
    int priority;
} KvManifest;

void kv_plugins_load(void);
/* The plugin for a file, by MIME type and then by extension; NULL if none. */
const KvManifest *kv_plugins_for(const char *mime, const char *name);

typedef struct KvProc KvProc;
/* Called for every message; msg is NULL once the plugin is gone (then the
 * KvProc must be stopped). */
typedef void (*KvProcCb)(KvProc *p, const KvpMsg *msg, gpointer ud);

KvProc *kv_proc_start(const KvManifest *man, KvProcCb cb, gpointer ud);
const KvManifest *kv_proc_manifest(const KvProc *p);
/* Takes the message; fd (or -1) is sent along and stays the caller's. */
void kv_proc_send(KvProc *p, GString *msg, int fd);
void kv_proc_stop(KvProc *p);

/* ---- kv_folder.c: the files next to the one shown ---- */

typedef struct KvFolder KvFolder;
typedef void (*KvFolderChanged)(gpointer ud);

KvFolder *kv_folder_new(GFile *dir, const char *current, KvFolderChanged cb, gpointer ud);
void kv_folder_free(KvFolder *f);
int kv_folder_count(const KvFolder *f);
int kv_folder_pos(const KvFolder *f); /* 0-based, -1: none */
const char *kv_folder_current(const KvFolder *f);
GFile *kv_folder_dir(const KvFolder *f);
/* The name at an index, wrapping around. */
const char *kv_folder_name(const KvFolder *f, int index);
/* Both wrap around; return the new current name (NULL: empty folder). */
const char *kv_folder_step(KvFolder *f, int delta);
const char *kv_folder_goto(KvFolder *f, int index);

/* ---- kv_window.c ---- */

void kv_window_run(GFile *file); /* file may be NULL */

#endif
