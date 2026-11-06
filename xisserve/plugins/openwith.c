/*
 * openwith.c - search plugin: when the whole query is the name of a file
 * or folder, offers one result per program associated with it -- the
 * same list as kifiles' "Open With" menu (the default program first, then
 * every other one registered for the item's content type) -- and opening
 * a result runs that program on the item.
 *
 * "Is the name of a file or folder" is decided with a few stat calls, not
 * a search, so it costs nothing per keystroke: the query as a path (/abs,
 * ~/x, file://, or relative to $HOME), then the bare name inside $HOME and
 * the user's Desktop/Documents/Downloads/Music/Pictures/Videos folders,
 * then an exact (case-insensitive) match on the file name of a recent
 * file. Anything deeper is for kifind.
 *
 * xisserve.conf: "OPENWITH\tmax_files" (default 3) caps how many items a
 * name can resolve to, "OPENWITH\tmax_apps" (default 6) how many programs
 * are listed for each; "PLUGIN\topenwith\tno" turns it off.
 */
#include "../xisserve.h"

#include <gio/gio.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MIN_QUERY_CHARS 2

typedef struct {
    GAppInfo *app;
    char *path;
} OpenWithJob;

static void job_free(gpointer p)
{
    OpenWithJob *j = p;
    g_object_unref(j->app);
    g_free(j->path);
    g_free(j);
}

static void job_run(ResultEntry *self)
{
    OpenWithJob *j = self->activate_data;
    GFile *f = g_file_new_for_path(j->path);
    GList *files = g_list_append(NULL, f);
    GError *err = NULL;
    if (!g_app_info_launch(j->app, files, NULL, &err)) {
        g_printerr("xisserve: openwith: could not open '%s': %s\n", j->path, err ? err->message : "?");
        g_clear_error(&err);
    }
    g_list_free(files);
    g_object_unref(f);
}

/* Copies `s` into dst (dstsz bytes), cutting at a character boundary and,
 * when it doesn't fit, keeping the END -- the part of a path that matters. */
static void fit_tail(char *dst, size_t dstsz, const char *s)
{
    if (strlen(s) < dstsz) {
        g_strlcpy(dst, s, dstsz);
        return;
    }
    const char *p = s + strlen(s) - (dstsz - 4);   /* room for the ellipsis + NUL */
    while (*p && ((unsigned char)*p & 0xC0) == 0x80) p++;
    snprintf(dst, dstsz, "…%s", p);
}

static gboolean already_have(GPtrArray *paths, const char *path)
{
    for (guint i = 0; i < paths->len; i++)
        if (!strcmp(g_ptr_array_index(paths, i), path)) return TRUE;
    return FALSE;
}

/* Adds the canonical absolute path of `cand` to `paths` if it exists. */
static void try_path(GPtrArray *paths, const char *cand, int max_files)
{
    if ((int)paths->len >= max_files) return;
    struct stat st;
    if (stat(cand, &st) != 0) return;
    /* Tidied (no "..", no double slashes) but not resolved: the user's own symlink path. */
    char *shown = g_canonicalize_filename(cand, NULL);
    if (already_have(paths, shown)) g_free(shown);
    else g_ptr_array_add(paths, shown);
}

static void collect_paths(const char *query, GPtrArray *paths, int max_files)
{
    const char *home = g_get_home_dir();
    if (g_str_has_prefix(query, "file://")) {
        char *p = g_filename_from_uri(query, NULL, NULL);
        if (p) { try_path(paths, p, max_files); g_free(p); }
        return;
    }
    if (query[0] == '/') {
        try_path(paths, query, max_files);
        return;
    }
    if (query[0] == '~' && (query[1] == '/' || query[1] == '\0')) {
        char *p = g_build_filename(home, query + (query[1] ? 2 : 1), NULL);
        try_path(paths, p, max_files);
        g_free(p);
        return;
    }
    if (strchr(query, '/')) {   /* relative: under $HOME */
        char *p = g_build_filename(home, query, NULL);
        try_path(paths, p, max_files);
        g_free(p);
        return;
    }

    static const GUserDirectory dirs[] = {
        G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DOWNLOAD,
        G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_VIDEOS,
    };
    char *p = g_build_filename(home, query, NULL);
    try_path(paths, p, max_files);
    g_free(p);
    for (size_t i = 0; i < G_N_ELEMENTS(dirs); i++) {
        const char *d = g_get_user_special_dir(dirs[i]);
        if (!d || !strcmp(d, home)) continue;
        p = g_build_filename(d, query, NULL);
        try_path(paths, p, max_files);
        g_free(p);
    }

    if ((int)paths->len < max_files) {
        char *q_cf = g_utf8_casefold(query, -1);
        GPtrArray *recent = xisserve_load_recent_xbel();
        for (guint i = 0; i < recent->len && (int)paths->len < max_files; i++) {
            RecentXbelItem *item = g_ptr_array_index(recent, i);
            char *base = g_path_get_basename(item->path);
            char *base_cf = g_utf8_casefold(base, -1);
            if (!strcmp(base_cf, q_cf)) try_path(paths, item->path, max_files);
            g_free(base_cf);
            g_free(base);
        }
        g_ptr_array_free(recent, TRUE);
        g_free(q_cf);
    }
}

static char *content_type_of(const char *path)
{
    GFile *f = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    char *ct = info ? g_strdup(g_file_info_get_content_type(info)) : NULL;
    if (info) g_object_unref(info);
    g_object_unref(f);
    return ct;
}

/* The default program first, then the rest, each once and only if it is
 * meant to be shown (no NoDisplay/hidden entries). */
static GList *apps_for(const char *ctype)
{
    GList *out = NULL;
    GAppInfo *def = g_app_info_get_default_for_type(ctype, FALSE);
    if (def && g_app_info_should_show(def)) out = g_list_append(out, g_object_ref(def));
    GList *all = g_app_info_get_all_for_type(ctype);
    for (GList *l = all; l; l = l->next) {
        GAppInfo *ai = l->data;
        if (!g_app_info_should_show(ai)) continue;
        if (def && g_app_info_equal(ai, def)) continue;
        out = g_list_append(out, g_object_ref(ai));
    }
    g_list_free_full(all, g_object_unref);
    if (def) g_object_unref(def);
    return out;
}

void plugin_openwith_search(const char *query, GPtrArray *results)
{
    if (g_utf8_strlen(query, -1) < MIN_QUERY_CHARS) return;
    int max_files = xisserve_config_get_int("OPENWITH", "max_files", 3);
    int max_apps = xisserve_config_get_int("OPENWITH", "max_apps", 6);
    if (max_files <= 0 || max_apps <= 0) return;

    char *q = g_strstrip(g_strdup(query));
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    if (q[0]) collect_paths(q, paths, max_files);
    g_free(q);

    const char *home = g_get_home_dir();
    size_t hlen = strlen(home);
    for (guint i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        char *ctype = content_type_of(path);
        if (!ctype) continue;
        GList *apps = apps_for(ctype);
        g_free(ctype);

        char *base = g_path_get_basename(path);
        char shown[PATH_MAX + 2];
        if (!strncmp(path, home, hlen) && (path[hlen] == '/' || !path[hlen]))
            snprintf(shown, sizeof(shown), "~%s", path + hlen);
        else
            g_strlcpy(shown, path, sizeof(shown));

        int n = 0;
        for (GList *l = apps; l && n < max_apps; l = l->next, n++) {
            GAppInfo *ai = l->data;
            ResultEntry *e = g_new0(ResultEntry, 1);
            char title[256];
            snprintf(title, sizeof(title), _("Abrir com %s"), g_app_info_get_display_name(ai));
            fit_tail(e->name, sizeof(e->name), title);
            char sub[2 * PATH_MAX + 8];
            snprintf(sub, sizeof(sub), "%s - %s", base, shown);
            fit_tail(e->subtitle, sizeof(e->subtitle), sub);
            GIcon *gi = g_app_info_get_icon(ai);
            char *spec = gi ? g_icon_to_string(gi) : NULL;
            if (spec) {
                g_strlcpy(e->icon_spec, spec, sizeof(e->icon_spec));
                e->icon = xisserve_resolve_icon(spec, XISSERVE_ICON_PX);
                g_free(spec);
            }
            OpenWithJob *j = g_new0(OpenWithJob, 1);
            j->app = g_object_ref(ai);
            j->path = g_strdup(path);
            e->activate_fn = job_run;
            e->activate_data = j;
            e->activate_data_free = job_free;
            g_ptr_array_add(results, e);
        }
        g_free(base);
        g_list_free_full(apps, g_object_unref);
    }
    g_ptr_array_free(paths, TRUE);
}
