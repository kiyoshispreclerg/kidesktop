/* kf_clip.c - file clipboard, interoperable with GNOME/Thunar/PCManFM
 * (x-special/gnome-copied-files), KDE (application/x-kde-cutselection) and
 * plain text/uri-list consumers. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

enum { T_GNOME, T_URIS, T_TEXT, T_KDECUT };

typedef struct {
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

static void clip_clear(GtkClipboard *cb, gpointer data)
{
    (void)cb;
    ClipData *d = data;
    g_strfreev(d->uris);
    g_free(d);
}

void kf_clip_set(KfApp *app, GList *files, gboolean cut)
{
    if (!files)
        return;
    ClipData *d = g_new0(ClipData, 1);
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
    if (!gtk_clipboard_set_with_data(cb, targets, G_N_ELEMENTS(targets), clip_get, clip_clear, d))
        clip_clear(cb, d);
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
