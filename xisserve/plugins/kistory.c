/*
 * kistory.c - search plugin: matches the query against kistoryd's activity
 * log (../../kistory/, $XDG_DATA_HOME/kistory/events/YYYY-MM-DD.tsv) --
 * files the user opened and window titles they had focused, most recent
 * first. A file result opens it with xdg-open (only files that still
 * exist are offered); a window result activates that window when one with
 * the same title is still open, and otherwise starts its program again.
 *
 * The log is read straight from the day files, like the kistory CLI, not
 * through kistoryd: it works whether the daemon runs or not. The last
 * KISTORY search_days (default 30) are parsed once into a de-duplicated
 * in-memory list and only re-read when a day file changed, so typing in
 * the launcher costs a scan of that list, not of the files.
 *
 * xisserve.conf: "KISTORY\tsearch_days\t<n>", "KISTORY\tsearch_max\t<n>"
 * (default 6), "PLUGIN\tkistory\tno" turns it off.
 */
#define _GNU_SOURCE   /* strptime */
#include "../xisserve.h"

#include <X11/Xatom.h>
#include <dirent.h>
#include <gdk/gdkx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MIN_QUERY_CHARS 3
#define RELOAD_CHECK_S 5

enum { HIT_FILE, HIT_WINDOW };

typedef struct {
    int kind;
    char *text;       /* path, or window title */
    char *text_cf;    /* casefolded, for matching */
    char *app;
    char *exe;
    time_t ts;        /* last time seen */
} Hit;

static GPtrArray *g_hits;      /* Hit*, newest first */
static char g_signature[256];  /* day-file names+mtimes the cache was built from */
static time_t g_checked_at;

static void hit_free(gpointer p)
{
    Hit *h = p;
    g_free(h->text);
    g_free(h->text_cf);
    g_free(h->app);
    g_free(h->exe);
    g_free(h);
}

static void events_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(out, outsz, "%s/kistory/events", xdg);
    else
        snprintf(out, outsz, "%s/.local/share/kistory/events", g_get_home_dir());
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Day files within the window, oldest first, and a signature of their
 * names/sizes/mtimes to tell whether the cache is stale. */
static GPtrArray *day_files(char *signature, size_t sigsz)
{
    char dir[PATH_MAX];
    events_dir(dir, sizeof(dir));
    int days = xisserve_config_get_int("KISTORY", "search_days", 30);
    time_t cutoff = time(NULL) - (time_t)days * 86400;
    struct tm tm;
    localtime_r(&cutoff, &tm);
    char oldest[16];
    strftime(oldest, sizeof(oldest), "%Y-%m-%d", &tm);

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) == 14 && !strcmp(e->d_name + 10, ".tsv") && strncmp(e->d_name, oldest, 10) >= 0)
                g_ptr_array_add(names, g_build_filename(dir, e->d_name, NULL));
        }
        closedir(d);
    }
    qsort(names->pdata, names->len, sizeof(char *), cmp_names);

    guint64 h = 1469598103934665603ULL;
    for (guint i = 0; i < names->len; i++) {
        struct stat st;
        if (stat(g_ptr_array_index(names, i), &st) != 0)
            continue;
        guint64 v[2] = { (guint64)st.st_mtime, (guint64)st.st_size };
        const unsigned char *b = (const unsigned char *)v;
        for (size_t k = 0; k < sizeof(v); k++)
            h = (h ^ b[k]) * 1099511628211ULL;
    }
    snprintf(signature, sigsz, "%u:%016llx", names->len, (unsigned long long)h);
    return names;
}

/* Splits a log line in place into its 8 tab-separated fields, unescaping
 * \t \n \\ (same format as kistory's ks_log.c). */
static int split_line(char *line, char **f)
{
    int n = 0;
    char *out = line, *start = line;
    for (char *p = line;; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *out++ = *p == 't' ? '\t' : *p == 'n' ? ' ' : *p;
            continue;
        }
        if (*p == '\t' || *p == '\n' || *p == '\0') {
            char c = *p;
            *out++ = '\0';
            if (n < 8)
                f[n++] = start;
            if (c != '\t')
                break;
            start = out;
            continue;
        }
        *out++ = *p;
    }
    for (int i = n; i < 8; i++)
        f[i] = "";
    return n;
}

static time_t parse_ts(const char *s)
{
    struct tm tm = { 0 };
    if (!strptime(s, "%Y-%m-%dT%H:%M:%S", &tm))
        return 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static void add_hit(GHashTable *seen, int kind, const char *text, const char *app, const char *exe, time_t ts)
{
    if (!text[0])
        return;
    char *key = g_strdup_printf("%d\t%s\t%s", kind, app, text);
    Hit *h = g_hash_table_lookup(seen, key);
    if (h) {
        if (ts > h->ts)
            h->ts = ts;
        g_free(key);
        return;
    }
    h = g_new0(Hit, 1);
    h->kind = kind;
    h->text = g_strdup(text);
    h->text_cf = g_utf8_casefold(text, -1);
    h->app = g_strdup(app);
    h->exe = g_strdup(exe);
    h->ts = ts;
    g_hash_table_insert(seen, key, h);
}

static gint newest_first(gconstpointer a, gconstpointer b)
{
    const Hit *x = *(Hit *const *)a, *y = *(Hit *const *)b;
    return x->ts < y->ts ? 1 : x->ts > y->ts ? -1 : 0;
}

static void reload_if_stale(void)
{
    time_t now = time(NULL);
    if (g_hits && now - g_checked_at < RELOAD_CHECK_S)
        return;
    g_checked_at = now;

    char sig[256];
    GPtrArray *files = day_files(sig, sizeof(sig));
    if (g_hits && !strcmp(sig, g_signature)) {
        g_ptr_array_free(files, TRUE);
        return;
    }
    snprintf(g_signature, sizeof(g_signature), "%s", sig);

    /* Values owned by the hash table until they move into g_hits. */
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *line = NULL;
    size_t cap = 0;
    for (guint i = 0; i < files->len; i++) {
        FILE *fp = fopen(g_ptr_array_index(files, i), "r");
        if (!fp)
            continue;
        while (getline(&line, &cap, fp) > 0) {
            char *f[8];
            split_line(line, f);
            /* ts kind app exe desktop output subject detail */
            time_t ts = parse_ts(f[0]);
            if (!strcmp(f[1], "file")) {
                add_hit(seen, HIT_FILE, f[6], f[2], f[3], ts);
            } else if (!strcmp(f[1], "win_open")) {
                add_hit(seen, HIT_WINDOW, f[6], f[2], f[3], ts);
            } else if (!strcmp(f[1], "focus")) {
                /* Every distinct title the window showed, " | "-separated. */
                char **titles = g_strsplit(f[7], " | ", -1);
                for (int t = 0; titles[t]; t++)
                    add_hit(seen, HIT_WINDOW, titles[t], f[2], f[3], ts);
                g_strfreev(titles);
            }
        }
        fclose(fp);
    }
    free(line);
    g_ptr_array_free(files, TRUE);

    if (g_hits)
        g_ptr_array_free(g_hits, TRUE);
    g_hits = g_ptr_array_new_with_free_func(hit_free);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, seen);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(g_hits, v);
    g_hash_table_destroy(seen);
    g_ptr_array_sort(g_hits, newest_first);
}

/* ---- activating a window hit ------------------------------------------- */

typedef struct {
    Window win;
    char *title;
    char *cls;
} OpenWin;

static void open_win_free(gpointer p)
{
    OpenWin *o = p;
    g_free(o->title);
    g_free(o->cls);
    g_free(o);
}

/* Title and WM_CLASS of every managed window, read once per search. */
static GPtrArray *open_windows(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(open_win_free);
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    gdk_error_trap_push();   /* other programs' windows can vanish mid-scan */
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_NET_CLIENT_LIST", False), 0, 4096,
                           False, XA_WINDOW, &type, &format, &n, &after, &data) == Success && data) {
        for (unsigned long i = 0; i < n; i++) {
            Window w = (Window)((long *)(void *)data)[i];
            /* _NET_WM_NAME, else WM_NAME (Xt/Xaw apps set only that),
             * the same order kistoryd logged the title in. */
            unsigned char *name = NULL;
            unsigned long nn = 0;
            if (XGetWindowProperty(dpy, w, net_name, 0, 1024, False, AnyPropertyType, &type, &format, &nn, &after,
                                   &name) != Success || !name || !nn) {
                if (name)
                    XFree(name);
                name = NULL;
                if (XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 1024, False, AnyPropertyType, &type, &format, &nn,
                                       &after, &name) != Success || !name)
                    continue;
            }
            OpenWin *o = g_new0(OpenWin, 1);
            o->win = w;
            o->title = g_strdup((char *)name);
            XFree(name);
            XClassHint ch;
            if (XGetClassHint(dpy, w, &ch)) {
                o->cls = g_strdup(ch.res_class ? ch.res_class : "");
                if (ch.res_name) XFree(ch.res_name);
                if (ch.res_class) XFree(ch.res_class);
            } else {
                o->cls = g_strdup("");
            }
            g_ptr_array_add(out, o);
        }
    }
    if (data)
        XFree(data);
    gdk_error_trap_pop();
    return out;
}

/* An open window whose title is exactly `title` (and whose WM_CLASS is
 * `app`, when known), or None. */
static Window find_open_window(GPtrArray *wins, const char *title, const char *app)
{
    for (guint i = 0; i < wins->len; i++) {
        OpenWin *o = g_ptr_array_index(wins, i);
        if (!strcmp(o->title, title) && (!app[0] || !strcmp(o->cls, app)))
            return o->win;
    }
    return None;
}

static void activate_window(ResultEntry *e)
{
    Window w = (Window)GPOINTER_TO_SIZE(e->activate_data);
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;   /* source: pager/tool */
    ev.xclient.data.l[1] = CurrentTime;
    gdk_error_trap_push();
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
    gdk_error_trap_pop();
}

/* ---- search ------------------------------------------------------------- */

static void when_string(time_t ts, char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&ts, &a);
    localtime_r(&now, &b);
    strftime(out, outsz, a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "hoje %H:%M" : "%d/%m %H:%M", &a);
}

static GdkPixbuf *app_icon(const char *app, const char *exe)
{
    char *lower = g_ascii_strdown(app, -1);
    GdkPixbuf *pb = lower[0] ? xisserve_resolve_icon(lower, XISSERVE_ICON_PX) : NULL;
    g_free(lower);
    if (!pb && exe[0]) {
        char *base = g_path_get_basename(exe);
        pb = xisserve_resolve_icon(base, XISSERVE_ICON_PX);
        g_free(base);
    }
    return pb ? pb : xisserve_resolve_icon("window-new", XISSERVE_ICON_PX);
}

void plugin_kistory_search(const char *query, GPtrArray *results)
{
    int max_results = xisserve_config_get_int("KISTORY", "search_max", 6);
    if (max_results <= 0 || g_utf8_strlen(query, -1) < MIN_QUERY_CHARS)
        return;
    reload_if_stale();
    if (!g_hits)
        return;

    gchar *query_cf = g_utf8_casefold(query, -1);
    GPtrArray *wins = NULL;   /* read on the first window hit only */
    int shown = 0;
    for (guint i = 0; i < g_hits->len && shown < max_results; i++) {
        Hit *h = g_ptr_array_index(g_hits, i);
        if (!strstr(h->text_cf, query_cf))
            continue;
        char when[32];
        when_string(h->ts, when, sizeof(when));
        ResultEntry *e = g_new0(ResultEntry, 1);
        e->from_desktop = FALSE;

        if (h->kind == HIT_FILE) {
            if (h->text[0] != '/' || !g_file_test(h->text, G_FILE_TEST_EXISTS)) {
                g_free(e);
                continue;
            }
            char *base = g_path_get_basename(h->text);
            char *dir = g_path_get_dirname(h->text);
            snprintf(e->name, sizeof(e->name), "%s", base);
            snprintf(e->subtitle, sizeof(e->subtitle), "Hist\xc3\xb3rico \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s",
                     h->app[0] ? h->app : "arquivo", when, dir);
            char quoted[1200];
            shell_quote(h->text, quoted, sizeof(quoted));
            snprintf(e->exec, sizeof(e->exec), "xdg-open %s", quoted);
            e->icon = xisserve_resolve_icon(g_file_test(h->text, G_FILE_TEST_IS_DIR) ? "folder" : "document-open-recent",
                                            XISSERVE_ICON_PX);
            g_free(base);
            g_free(dir);
        } else {
            if (!wins)
                wins = open_windows();
            Window open = find_open_window(wins, h->text, h->app);
            snprintf(e->name, sizeof(e->name), "%s", h->text);
            snprintf(e->subtitle, sizeof(e->subtitle), "Hist\xc3\xb3rico \xc2\xb7 %s \xc2\xb7 %s%s",
                     h->app[0] ? h->app : "janela", when, open ? " \xc2\xb7 aberta" : "");
            if (open) {
                e->activate_fn = activate_window;
                e->activate_data = GSIZE_TO_POINTER((gsize)open);
            } else if (h->exe[0]) {
                char quoted[600];
                shell_quote(h->exe, quoted, sizeof(quoted));
                snprintf(e->exec, sizeof(e->exec), "%s", quoted);
            } else {
                g_free(e);
                continue;   /* closed and nothing to reopen it with */
            }
            e->icon = app_icon(h->app, h->exe);
        }
        g_ptr_array_add(results, e);
        shown++;
    }
    if (wins)
        g_ptr_array_free(wins, TRUE);
    g_free(query_cf);
}
