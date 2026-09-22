/*
 * icons.c - icon resolution for every view, plus the progressive filler
 * that keeps it off the critical path.
 *
 * Resolution itself (xisserve_resolve_icon(), which used to live in
 * xisserve.c) is unchanged: a process-wide cache keyed by the raw Icon=
 * spec, hits and misses alike. What moved here with it is the reason
 * this file exists at all -- resolving is *expensive*, and for a long
 * while xisserve paid for all of it up front.
 *
 * Measured on a normal desktop (561 apps in the on-disk apps cache):
 * gtk_init 7ms, reading and parsing the whole apps cache 0.1ms, reading
 * all 573 .desktop files from disk 2ms, building 564 image menu items
 * 7ms -- and resolving their 552 icons 570-950ms. Every other cost in
 * opening the launcher or the --applications menu is noise next to icon
 * theme lookups and PNG/SVG decodes, and the scan-time code was doing
 * all 552 of them before anything appeared on screen, including the
 * hundreds of icons for apps the user would never scroll to.
 *
 * So nothing resolves an icon at scan/cache-load time any more (see
 * xisserve.c's parse_desktop_file()/load_apps_cache_if_fresh(), which
 * now only keep the spec). Instead a view goes on screen immediately
 * with whatever icons are already in the cache, hands the rest to an
 * IconJob here, and its icons appear in place over the next few frames.
 *
 * Both consumers -- the launcher's results list and the --applications
 * menu -- use the same IconJob rather than each rolling its own lazy
 * scheme, so "icons fill in progressively" means one behavior, one
 * chunk size, one cancellation rule everywhere.
 */
#include <gtk/gtk.h>

#include "xisserve.h"

/* spec -> resolved GdkPixbuf* (or the NULL "nothing resolves this"
 * result), keyed exactly as passed to xisserve_resolve_icon(). g_hash_
 * table_lookup_extended() (not a plain lookup()) is what lets a cached
 * NULL be told apart from "not in the cache yet" without a sentinel. */
static GHashTable *g_icon_cache;

GdkPixbuf *xisserve_resolve_icon(const char *spec, int size)
{
    if (!spec || !spec[0]) return NULL;
    if (!g_icon_cache) g_icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    gpointer cached = NULL;
    if (g_hash_table_lookup_extended(g_icon_cache, spec, NULL, &cached)) {
        return cached ? GDK_PIXBUF(g_object_ref(cached)) : NULL;
    }

    GdkPixbuf *pixbuf = NULL;
    if (spec[0] == '/') {
        pixbuf = gdk_pixbuf_new_from_file_at_size(spec, size, size, NULL);
    } else {
        pixbuf = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), spec, size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
    /* The cache keeps its own reference (or NULL); every caller,
     * including this first one, gets back a fresh ref it owns. */
    g_hash_table_insert(g_icon_cache, g_strdup(spec), pixbuf);
    return pixbuf ? g_object_ref(pixbuf) : NULL;
}

gboolean xisserve_icon_resolved(const char *spec, GdkPixbuf **out)
{
    *out = NULL;
    if (!spec || !spec[0]) return TRUE; /* "no icon" is a settled answer, not pending work */
    if (!g_icon_cache) return FALSE;

    gpointer cached = NULL;
    if (!g_hash_table_lookup_extended(g_icon_cache, spec, NULL, &cached)) return FALSE;
    if (cached) *out = GDK_PIXBUF(g_object_ref(cached));
    return TRUE;
}

/* ---- progressive fill ----------------------------------------------- */

typedef struct {
    ResultEntry *entry;
    gpointer target;
} IconJobItem;

struct XisserveIconJob {
    GArray *items;     /* IconJobItem, entries and targets both borrowed */
    guint next;        /* index of the first item not yet resolved */
    guint idle_id;
    XisserveIconApplyFn apply;
    gpointer user_data;
};

/* How long one idle pass is allowed to spend resolving. A single icon
 * is ~1ms (an SVG can be several), so a fixed item count would make the
 * worst case unbounded; a deadline keeps each pass shorter than a frame
 * no matter what the theme throws at it, and at least one item always
 * gets resolved so a pathological icon can't stall the job forever. */
#define ICON_FILL_BUDGET_US 8000

static gboolean icon_job_step(gpointer data)
{
    XisserveIconJob *job = data;
    gint64 deadline = g_get_monotonic_time() + ICON_FILL_BUDGET_US;

    do {
        IconJobItem *it = &g_array_index(job->items, IconJobItem, job->next++);
        ResultEntry *e = it->entry;
        /* The entry owns the ref (result_entry_free() drops it); apply
         * only borrows it, exactly like the already-resolved path in
         * the caller does. */
        if (!e->icon && e->icon_spec[0]) e->icon = xisserve_resolve_icon(e->icon_spec, XISSERVE_ICON_PX);
        if (e->icon) job->apply(it->target, e->icon, job->user_data);
    } while (job->next < job->items->len && g_get_monotonic_time() < deadline);

    if (job->next < job->items->len) return TRUE;
    job->idle_id = 0;
    return FALSE;
}

XisserveIconJob *xisserve_icon_job_new(XisserveIconApplyFn apply, gpointer user_data)
{
    XisserveIconJob *job = g_new0(XisserveIconJob, 1);
    job->items = g_array_new(FALSE, FALSE, sizeof(IconJobItem));
    job->apply = apply;
    job->user_data = user_data;
    return job;
}

void xisserve_icon_job_add(XisserveIconJob *job, ResultEntry *entry, gpointer target)
{
    IconJobItem it = { entry, target };
    g_array_append_val(job->items, it);
}

void xisserve_icon_job_start(XisserveIconJob **job)
{
    XisserveIconJob *j = *job;
    if (j->items->len == 0) { /* everything was cached already -- nothing to fill in */
        xisserve_icon_job_cancel(job);
        return;
    }
    /* Below GDK_PRIORITY_REDRAW on purpose: whatever is already on
     * screen keeps painting and scrolling smoothly, and icons land in
     * the gaps between frames. */
    j->idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, icon_job_step, j, NULL);
}

void xisserve_icon_job_cancel(XisserveIconJob **job)
{
    XisserveIconJob *j = *job;
    if (!j) return;
    *job = NULL;
    if (j->idle_id) g_source_remove(j->idle_id);
    g_array_free(j->items, TRUE);
    g_free(j);
}
