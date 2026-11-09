/* kf_clip.c - file clipboard, interoperable with GNOME/Thunar/PCManFM
 * (x-special/gnome-copied-files), KDE (application/x-kde-cutselection) and
 * plain text/uri-list consumers. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

enum { T_GNOME, T_URIS, T_TEXT, T_KDECUT };

typedef struct {
    KfApp *app;
    GStrv uris;
    gboolean cut;
} ClipData;

static GdkAtom atom_gnome(void) { return gdk_atom_intern_static_string("x-special/gnome-copied-files"); }

static void clip_get(GtkClipboard *cb, GtkSelectionData *sel, guint info, gpointer data)
{
    (void)cb;
    ClipData *d = data;
    GString *out = g_string_new(NULL);
    switch (info) {
    case T_GNOME:
        g_string_append(out, d->cut ? "cut" : "copy");
        for (char **u = d->uris; *u; u++) {
            g_string_append_c(out, '\n');
            g_string_append(out, *u);
        }
        break;
    case T_URIS:
        for (char **u = d->uris; *u; u++) {
            g_string_append(out, *u);
            g_string_append(out, "\r\n");
        }
        break;
    case T_TEXT:
        for (char **u = d->uris; *u; u++) {
            char *path = g_filename_from_uri(*u, NULL, NULL);
            if (out->len)
                g_string_append_c(out, '\n');
            g_string_append(out, path ? path : *u);
            g_free(path);
        }
        break;
    case T_KDECUT:
        g_string_append(out, d->cut ? "1" : "0");
        break;
    }
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar *)out->str, out->len);
    g_string_free(out, TRUE);
}

static void cut_redraw(KfApp *app)
{
    GList *tabs = kf_tabs_all(app);
    for (GList *t = tabs; t; t = t->next)
        for (int i = 0; i < 2; i++) {
            KfPane *p = ((KfTab *)t->data)->panes[i];
            if (p) {
                gtk_widget_queue_draw(p->iconview);
                gtk_widget_queue_draw(p->treeview);
            }
        }
    g_list_free(tabs);
}

gboolean kf_clip_is_cut(KfApp *app, GFile *f)
{
    if (!app->cut_uris)
        return FALSE;
    char *uri = g_file_get_uri(f);
    gboolean r = g_hash_table_contains(app->cut_uris, uri);
    g_free(uri);
    return r;
}

static void clip_clear(GtkClipboard *cb, gpointer data)
{
    (void)cb;
    /* Not the end of the cut: a clipboard manager re-owning the selection also lands
     * here. owner_changed() decides from what the clipboard holds now. */
    ClipData *d = data;
    g_strfreev(d->uris);
    g_free(d);
}

static void cut_contents(GtkClipboard *cb, GtkSelectionData *sel, gpointer data)
{
    (void)cb;
    KfApp *app = data;
    GHashTable *set = NULL;
    const char *txt = gtk_selection_data_get_length(sel) > 0 ? (const char *)gtk_selection_data_get_data(sel) : NULL;
    if (txt && g_str_has_prefix(txt, "cut\n")) {
        char **lines = g_strsplit(txt, "\n", -1);
        set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (char **l = lines + 1; *l; l++) {
            char *u = g_strstrip(*l);
            if (*u)
                g_hash_table_add(set, g_strdup(u));
        }
        g_strfreev(lines);
    }
    if (app->cut_uris)
        g_hash_table_destroy(app->cut_uris);
    app->cut_uris = set;
    cut_redraw(app);
}

static void owner_changed(GtkClipboard *cb, GdkEvent *ev, gpointer data)
{
    (void)ev;
    gtk_clipboard_request_contents(cb, gdk_atom_intern_static_string("x-special/gnome-copied-files"),
                                   cut_contents, data);
}

void kf_clip_set(KfApp *app, GList *files, gboolean cut)
{
    if (!files)
        return;
    ClipData *d = g_new0(ClipData, 1);
    d->app = app;
    d->cut = cut;
    d->uris = g_new0(char *, g_list_length(files) + 1);
    int i = 0;
    for (GList *l = files; l; l = l->next)
        d->uris[i++] = g_file_get_uri(l->data);
    static const GtkTargetEntry targets[] = {
        { (gchar *)"x-special/gnome-copied-files", 0, T_GNOME },
        { (gchar *)"text/uri-list", 0, T_URIS },
        { (gchar *)"UTF8_STRING", 0, T_TEXT },
        { (gchar *)"application/x-kde-cutselection", 0, T_KDECUT },
    };
    GtkClipboard *cb = gtk_widget_get_clipboard(app->win, GDK_SELECTION_CLIPBOARD);
    if (!gtk_clipboard_set_with_data(cb, targets, G_N_ELEMENTS(targets), clip_get, clip_clear, d)) {
        clip_clear(cb, d);
        return;
    }
    static gboolean watching;
    if (!watching) {
        watching = TRUE;
        g_signal_connect(cb, "owner-change", G_CALLBACK(owner_changed), app);
    }
    if (cut) {
        if (app->cut_uris)
            g_hash_table_destroy(app->cut_uris);
        app->cut_uris = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (char **u = d->uris; *u; u++)
            g_hash_table_add(app->cut_uris, g_strdup(*u));
        cut_redraw(app);
    }
}

typedef struct {
    KfApp *app;
    GFile *dest;
} PasteCtx;

static GList *parse_uris(char **lines)
{
    GList *files = NULL;
    for (; *lines; lines++) {
        char *l = g_strstrip(*lines);
        if (!*l || *l == '#')
            continue;
        files = g_list_append(files, g_file_new_for_uri(l));
    }
    return files;
}

static void paste_run(PasteCtx *c, GList *files, gboolean cut)
{
    if (files) {
        kf_ops_transfer(c->app, files, c->dest, cut);
        if (cut)
            gtk_clipboard_clear(gtk_widget_get_clipboard(c->app->win, GDK_SELECTION_CLIPBOARD));
    }
    kf_files_free(files);
    g_object_unref(c->dest);
    g_free(c);
}

static void on_uris(GtkClipboard *cb, GtkSelectionData *sel, gpointer data)
{
    (void)cb;
    PasteCtx *c = data;
    GList *files = NULL;
    char **uris = gtk_selection_data_get_uris(sel);
    if (uris) {
        files = parse_uris(uris);
        g_strfreev(uris);
    }
    paste_run(c, files, FALSE);
}

static void on_gnome(GtkClipboard *cb, GtkSelectionData *sel, gpointer data)
{
    PasteCtx *c = data;
    gint len = gtk_selection_data_get_length(sel);
    if (len <= 0) {
        /* Not from a GNOME-style source: try the plain URI list. */
        gtk_clipboard_request_contents(cb, gdk_atom_intern_static_string("text/uri-list"), on_uris, c);
        return;
    }
    char *text = g_strndup((const char *)gtk_selection_data_get_data(sel), len);
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    gboolean cut = lines[0] && !g_strcmp0(g_strstrip(lines[0]), "cut");
    GList *files = lines[0] && lines[1] ? parse_uris(lines + 1) : NULL;
    g_strfreev(lines);
    paste_run(c, files, cut);
}

void kf_clip_paste(KfApp *app, GFile *dest_dir)
{
    PasteCtx *c = g_new0(PasteCtx, 1);
    c->app = app;
    c->dest = g_object_ref(dest_dir);
    gtk_clipboard_request_contents(gtk_widget_get_clipboard(app->win, GDK_SELECTION_CLIPBOARD),
                                   atom_gnome(), on_gnome, c);
}
