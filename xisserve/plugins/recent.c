/*
 * recent.c - search plugin: matches the typed query against the XDG
 * "recent files" list (recently-used.xbel) by filename or full path --
 * unlike xisserve.c's own "recent files opened with this app"
 * context-menu section (see xisserve_load_recent_xbel() in xisserve.h,
 * which this reuses), this isn't scoped to one app: any recent file or
 * folder whose name or path contains the query matches. Opened via
 * `xdg-open` since a match may have been written by any app.
 */
#include "../xisserve.h"

#include <stdio.h>
#include <string.h>

static GdkPixbuf *recent_icon(void)
{
    return xisserve_resolve_icon("document-open-recent", XISSERVE_ICON_PX);
}

void plugin_recent_search(const char *query, GPtrArray *results)
{
    /* Same RECENT config section as xisserve.c's context-menu count,
     * but its own key -- the two lists serve different purposes and a
     * user may want them sized differently. */
    int max_results = xisserve_config_get_int("RECENT", "search_max", 6);
    if (max_results <= 0) return;

    gchar *query_cf = g_utf8_casefold(query, -1);
    GPtrArray *recent = xisserve_load_recent_xbel();
    int shown = 0;
    for (guint i = 0; i < recent->len && shown < max_results; i++) {
        RecentXbelItem *item = g_ptr_array_index(recent, i);
        gchar *path_cf = g_utf8_casefold(item->path, -1);
        gboolean match = strstr(path_cf, query_cf) != NULL;
        g_free(path_cf);
        if (!match) continue;

        ResultEntry *e = g_new0(ResultEntry, 1);
        char *base = g_path_get_basename(item->path);
        snprintf(e->name, sizeof(e->name), "%s", base);
        g_free(base);
        snprintf(e->subtitle, sizeof(e->subtitle), "%s", item->path);
        char quoted[1200];
        shell_quote(item->path, quoted, sizeof(quoted));
        snprintf(e->exec, sizeof(e->exec), "xdg-open %s", quoted);
        e->from_desktop = FALSE;
        e->icon = recent_icon();
        g_ptr_array_add(results, e);
        shown++;
    }
    g_ptr_array_free(recent, TRUE);
    g_free(query_cf);
}
