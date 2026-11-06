/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The files next to the one shown, the ones a plugin handles, in natural
 * order (img2 before img10). A directory monitor keeps the list current. */
#include <string.h>

#include "kv.h"

#define RESCAN_DELAY_MS 300

struct KvFolder {
    GFile *dir;
    GPtrArray *names; /* char*, sorted */
    char *current;
    int index;
    GFileMonitor *monitor;
    guint rescan;
    KvFolderChanged cb;
    gpointer ud;
};

typedef struct {
    char *name, *key;
} Entry;

static int cmp_entry(gconstpointer a, gconstpointer b)
{
    return strcmp((*(Entry *const *)a)->key, (*(Entry *const *)b)->key);
}

static void entry_free(gpointer p)
{
    Entry *e = p;
    g_free(e->name);
    g_free(e->key);
    g_free(e);
}

static void scan(KvFolder *f)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
    GFileEnumerator *en = g_file_enumerate_children(
        f->dir, "standard::name,standard::content-type,standard::is-hidden,standard::type",
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    GFileInfo *info;
    while (en && (info = g_file_enumerator_next_file(en, NULL, NULL))) {
        const char *name = g_file_info_get_name(info);
        gboolean mine = f->current && !strcmp(name, f->current);
        if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY &&
            (mine || (!g_file_info_get_is_hidden(info) &&
                      kv_plugins_for(g_file_info_get_content_type(info), name)))) {
            Entry *e = g_new0(Entry, 1);
            e->name = g_strdup(name);
            e->key = g_utf8_collate_key_for_filename(name, -1);
            g_ptr_array_add(entries, e);
        }
        g_object_unref(info);
    }
    if (en)
        g_object_unref(en);
    g_ptr_array_sort(entries, cmp_entry);
    g_ptr_array_set_size(f->names, 0);
    f->index = -1;
    for (guint i = 0; i < entries->len; i++) {
        Entry *e = entries->pdata[i];
        if (f->current && !strcmp(e->name, f->current))
            f->index = (int)i;
        g_ptr_array_add(f->names, g_strdup(e->name));
    }
    g_ptr_array_free(entries, TRUE);
}

static gboolean do_rescan(gpointer data)
{
    KvFolder *f = data;
    f->rescan = 0;
    scan(f);
    if (f->index < 0 && f->names->len) {
        /* the shown file went away: stay put on the same place, as far as we can */
        f->index = 0;
        g_free(f->current);
        f->current = g_strdup(f->names->pdata[0]);
    }
    if (f->cb)
        f->cb(f->ud);
    return G_SOURCE_REMOVE;
}

static void on_dir_changed(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev,
                           gpointer data)
{
    (void)m;
    (void)file;
    (void)other;
    KvFolder *f = data;
    if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT || ev == G_FILE_MONITOR_EVENT_DELETED ||
        ev == G_FILE_MONITOR_EVENT_CREATED || ev == G_FILE_MONITOR_EVENT_MOVED_IN ||
        ev == G_FILE_MONITOR_EVENT_MOVED_OUT || ev == G_FILE_MONITOR_EVENT_RENAMED) {
        if (f->rescan)
            g_source_remove(f->rescan);
        f->rescan = g_timeout_add(RESCAN_DELAY_MS, do_rescan, f);
    }
}

KvFolder *kv_folder_new(GFile *dir, const char *current, KvFolderChanged cb, gpointer ud)
{
    KvFolder *f = g_new0(KvFolder, 1);
    f->dir = g_object_ref(dir);
    f->names = g_ptr_array_new_with_free_func(g_free);
    f->current = g_strdup(current);
    f->cb = cb;
    f->ud = ud;
    scan(f);
    if (!f->current && f->names->len) {
        f->index = 0;
        f->current = g_strdup(f->names->pdata[0]);
    }
    f->monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, NULL);
    if (f->monitor)
        g_signal_connect(f->monitor, "changed", G_CALLBACK(on_dir_changed), f);
    return f;
}

void kv_folder_free(KvFolder *f)
{
    if (!f)
        return;
    if (f->rescan)
        g_source_remove(f->rescan);
    if (f->monitor)
        g_object_unref(f->monitor);
    g_ptr_array_free(f->names, TRUE);
    g_object_unref(f->dir);
    g_free(f->current);
    g_free(f);
}

int kv_folder_count(const KvFolder *f)
{
    return (int)f->names->len;
}

int kv_folder_pos(const KvFolder *f)
{
    return f->index;
}

const char *kv_folder_current(const KvFolder *f)
{
    return f->current;
}

GFile *kv_folder_dir(const KvFolder *f)
{
    return f->dir;
}

const char *kv_folder_goto(KvFolder *f, int index)
{
    if (!f->names->len)
        return NULL;
    int n = (int)f->names->len;
    f->index = ((index % n) + n) % n;
    g_free(f->current);
    f->current = g_strdup(f->names->pdata[f->index]);
    return f->current;
}

const char *kv_folder_step(KvFolder *f, int delta)
{
    return kv_folder_goto(f, (f->index < 0 ? 0 : f->index) + delta);
}
