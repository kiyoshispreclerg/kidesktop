/* kf_typeahead.c - typing a name's first letters selects the first matching
 * item of the current folder.
 *
 * Keys typed within KF_TA_TIMEOUT ms of each other add to the prefix; the
 * same letter typed again steps to the next item starting with it (wrapping),
 * so "d d d" walks through the d's. Matching ignores case and accents. Plain
 * keys only: anything with Ctrl/Alt/Super is left to the accelerators. */
#include "kf.h"
#include <string.h>

#define KF_TA_TIMEOUT 1000

/* Case- and accent-insensitive form of `s`, for prefix comparisons. */
static char *fold(const char *s)
{
    char *nfd = g_utf8_normalize(s, -1, G_NORMALIZE_NFD);
    if (!nfd)
        return g_utf8_casefold(s, -1);
    GString *out = g_string_sized_new(strlen(nfd));
    for (const char *c = nfd; *c; c = g_utf8_next_char(c)) {
        gunichar u = g_utf8_get_char(c);
        if (!g_unichar_ismark(u))
            g_string_append_unichar(out, u);
    }
    g_free(nfd);
    char *r = g_utf8_casefold(out->str, -1);
    g_string_free(out, TRUE);
    return r;
}

static GtkTreePath *cursor_path(KfPane *p)
{
    GtkTreePath *path = NULL;
    if (p->view == KF_VIEW_ICONS)
        gtk_icon_view_get_cursor(GTK_ICON_VIEW(p->iconview), &path, NULL);
    else
        gtk_tree_view_get_cursor(GTK_TREE_VIEW(p->treeview), &path, NULL);
    return path;
}

/* Index of the first row at or after `start` (wrapping) whose name starts
 * with `prefix`, or -1. */
static int find_from(KfPane *p, const char *prefix, int start)
{
    int n = gtk_tree_model_iter_n_children(p->sort, NULL);
    for (int k = 0; k < n; k++) {
        int i = (start + k) % n;
        GtkTreeIter it;
        if (!gtk_tree_model_iter_nth_child(p->sort, &it, NULL, i))
            continue;
        char *name = NULL;
        gtk_tree_model_get(p->sort, &it, KF_COL_NAME, &name, -1);
        char *f = name ? fold(name) : NULL;
        gboolean hit = f && g_str_has_prefix(f, prefix);
        g_free(f);
        g_free(name);
        if (hit)
            return i;
    }
    return -1;
}

static void select_only(KfPane *p, int index)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(index, -1);
    if (p->view == KF_VIEW_ICONS) {
        GtkIconView *iv = GTK_ICON_VIEW(p->iconview);
        gtk_icon_view_unselect_all(iv);
        gtk_icon_view_select_path(iv, path);
        gtk_icon_view_set_cursor(iv, path, NULL, FALSE);
        gtk_icon_view_scroll_to_path(iv, path, FALSE, 0, 0);
    } else {
        GtkTreeView *tv = GTK_TREE_VIEW(p->treeview);
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(tv));
        gtk_tree_view_set_cursor(tv, path, NULL, FALSE);
        gtk_tree_view_scroll_to_cell(tv, path, NULL, FALSE, 0, 0);
    }
    gtk_tree_path_free(path);
}

/* Whether every character of `s` is the same one (and there is more than one). */
static gboolean one_letter_repeated(const char *s)
{
    gunichar first = g_utf8_get_char(s);
    const char *c = g_utf8_next_char(s);
    if (!*c)
        return FALSE;
    for (; *c; c = g_utf8_next_char(c))
        if (g_utf8_get_char(c) != first)
            return FALSE;
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    KfPane *p = data;
    if (ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_META_MASK))
        return FALSE;
    gunichar u = gdk_keyval_to_unicode(ev->keyval);
    if (!u || !g_unichar_isprint(u))
        return FALSE;
    gboolean fresh = ev->time - p->ta_time > KF_TA_TIMEOUT || !p->ta_buf->len;
    /* Space keeps its usual meaning unless it continues a name being typed. */
    if (u == ' ' && fresh)
        return FALSE;
    if (fresh)
        g_string_truncate(p->ta_buf, 0);
    p->ta_time = ev->time;
    g_string_append_unichar(p->ta_buf, u);

    gboolean step = !fresh && one_letter_repeated(p->ta_buf->str);
    char *prefix = fold(p->ta_buf->str);
    int start = 0;
    if (step) {
        /* look for the single letter, after the current item */
        char one[8] = { 0 };
        g_unichar_to_utf8(u, one);
        g_free(prefix);
        prefix = fold(one);
        GtkTreePath *cur = cursor_path(p);
        if (cur) {
            start = gtk_tree_path_get_indices(cur)[0] + 1;
            gtk_tree_path_free(cur);
        }
    }
    int i = find_from(p, prefix, start);
    if (i < 0 && step) {
        /* "dd" may also be a real prefix ("ddrescue") */
        g_free(prefix);
        prefix = fold(p->ta_buf->str);
        i = find_from(p, prefix, 0);
    }
    g_free(prefix);
    if (i >= 0)
        select_only(p, i);
    return TRUE;
}

void kf_typeahead_setup(KfPane *p)
{
    p->ta_buf = g_string_new(NULL);
    /* the tree view's own search popup would take the keys first */
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p->treeview), FALSE);
    g_signal_connect(p->iconview, "key-press-event", G_CALLBACK(on_key), p);
    g_signal_connect(p->treeview, "key-press-event", G_CALLBACK(on_key), p);
}

void kf_typeahead_free(KfPane *p)
{
    g_string_free(p->ta_buf, TRUE);
}
