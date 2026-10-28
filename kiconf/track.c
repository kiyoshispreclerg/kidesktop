/* kiconf - pending-changes tracking; see track.h. */
#include "track.h"

#include <string.h>

#define KEY_ROOT "kiconf-track-root"
#define KEY_BASE "kiconf-track-base"
#define KEY_IGNORE "kiconf-track-ignore"
#define KEY_STATE_FN "kiconf-track-state-fn"
#define KEY_STATE_DATA "kiconf-track-state-data"
#define KEY_STATE_BASE "kiconf-track-state-base"

/* Only plain values: an object/pointer column (a pixbuf, say) prints an
 * address that changes on every reload without anything being edited. */
static int plain_type(GType t)
{
    switch (G_TYPE_FUNDAMENTAL(t)) {
    case G_TYPE_STRING:
    case G_TYPE_BOOLEAN:
    case G_TYPE_CHAR:
    case G_TYPE_UCHAR:
    case G_TYPE_INT:
    case G_TYPE_UINT:
    case G_TYPE_LONG:
    case G_TYPE_ULONG:
    case G_TYPE_INT64:
    case G_TYPE_UINT64:
    case G_TYPE_FLOAT:
    case G_TYPE_DOUBLE:
    case G_TYPE_ENUM:
    case G_TYPE_FLAGS:
        return 1;
    default:
        return 0;
    }
}

static void append_rows(GString *s, GtkTreeModel *m, GtkTreeIter *parent, int depth)
{
    int ncols = gtk_tree_model_get_n_columns(m);
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_iter_children(m, &it, parent);
    while (ok) {
        g_string_append_printf(s, "\x1e%d", depth);
        for (int c = 0; c < ncols; c++) {
            GType t = gtk_tree_model_get_column_type(m, c);
            if (!plain_type(t)) {
                continue;
            }
            GValue v = G_VALUE_INIT;
            gtk_tree_model_get_value(m, &it, c, &v);
            gchar *txt = g_strdup_value_contents(&v);
            g_string_append_c(s, '\x1f');
            g_string_append(s, txt);
            g_free(txt);
            g_value_unset(&v);
        }
        append_rows(s, m, &it, depth + 1);
        ok = gtk_tree_model_iter_next(m, &it);
    }
}

/* The rows themselves, not what a filter/sort currently shows: typing in a
 * search box hides rows without editing anything. */
static char *model_value(GtkTreeModel *m)
{
    for (;;) {
        if (GTK_IS_TREE_MODEL_FILTER(m)) {
            m = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(m));
        } else if (GTK_IS_TREE_MODEL_SORT(m)) {
            m = gtk_tree_model_sort_get_model(GTK_TREE_MODEL_SORT(m));
        } else {
            break;
        }
    }
    GString *s = g_string_new("m");
    append_rows(s, m, NULL, 0);
    return g_string_free(s, FALSE);
}

/* The current value of an editable widget, or NULL if `w` isn't one.
 * Each kind is a leaf: its internal children (a combo's toggle button, a
 * file chooser button's own combo) are never looked at. */
static char *widget_value(GtkWidget *w)
{
    if (GTK_IS_ENTRY(w)) { /* spin buttons too: the text includes an unconfirmed typed value */
        return g_strdup(gtk_entry_get_text(GTK_ENTRY(w)));
    }
    if (GTK_IS_TOGGLE_BUTTON(w)) {
        return g_strdup(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
    }
    if (GTK_IS_COMBO_BOX(w)) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
        const char *text = GTK_IS_ENTRY(child) ? gtk_entry_get_text(GTK_ENTRY(child)) : "";
        return g_strdup_printf("%d\x1f%s", gtk_combo_box_get_active(GTK_COMBO_BOX(w)), text);
    }
    if (GTK_IS_COLOR_BUTTON(w)) {
        GdkColor c;
        gtk_color_button_get_color(GTK_COLOR_BUTTON(w), &c);
        return g_strdup_printf("%04x%04x%04x%04x", c.red, c.green, c.blue,
                               gtk_color_button_get_alpha(GTK_COLOR_BUTTON(w)));
    }
    if (GTK_IS_FONT_BUTTON(w)) {
        return g_strdup(gtk_font_button_get_font_name(GTK_FONT_BUTTON(w)));
    }
    if (GTK_IS_FILE_CHOOSER_BUTTON(w)) {
        gchar *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(w));
        return f ? f : g_strdup("");
    }
    if (GTK_IS_TEXT_VIEW(w)) {
        GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
        GtkTextIter s, e;
        gtk_text_buffer_get_bounds(b, &s, &e);
        return gtk_text_buffer_get_text(b, &s, &e, TRUE);
    }
    if (GTK_IS_RANGE(w) && !GTK_IS_SCROLLBAR(w)) {
        return g_strdup_printf("%.6g", gtk_range_get_value(GTK_RANGE(w)));
    }
    if (GTK_IS_TREE_VIEW(w)) {
        GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(w));
        return m ? model_value(m) : g_strdup("");
    }
    return NULL;
}

static char *state_value(GtkWidget *w)
{
    KiconfStateFn fn = (KiconfStateFn)g_object_get_data(G_OBJECT(w), KEY_STATE_FN);
    if (!fn) {
        return NULL;
    }
    char *v = fn(g_object_get_data(G_OBJECT(w), KEY_STATE_DATA));
    return v ? v : g_strdup("");
}

/* rebase=1: store every value as the new baseline. rebase=0: returns 1 as
 * soon as one differs from its baseline. A widget with no baseline yet
 * (created after the last rebase) isn't compared. */
static int walk(GtkWidget *w, int rebase)
{
    char *sv = state_value(w);
    if (sv) {
        if (rebase) {
            g_object_set_data_full(G_OBJECT(w), KEY_STATE_BASE, sv, g_free);
        } else {
            const char *base = g_object_get_data(G_OBJECT(w), KEY_STATE_BASE);
            int differs = base && strcmp(base, sv) != 0;
            g_free(sv);
            if (differs) {
                return 1;
            }
        }
    }
    if (g_object_get_data(G_OBJECT(w), KEY_IGNORE)) {
        return 0;
    }
    char *v = widget_value(w);
    if (v) {
        if (rebase) {
            g_object_set_data_full(G_OBJECT(w), KEY_BASE, v, g_free);
            return 0;
        }
        const char *base = g_object_get_data(G_OBJECT(w), KEY_BASE);
        int differs = base && strcmp(base, v) != 0;
        g_free(v);
        return differs;
    }
    int found = 0;
    if (GTK_IS_CONTAINER(w)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = children; l && !found; l = l->next) {
            found = walk(GTK_WIDGET(l->data), rebase);
        }
        g_list_free(children);
    }
    return found;
}

void kiconf_track_rebase(GtkWidget *w)
{
    if (w) {
        walk(w, 1);
    }
}

void kiconf_track_applied(GtkWidget *w)
{
    for (; w; w = gtk_widget_get_parent(w)) {
        if (g_object_get_data(G_OBJECT(w), KEY_ROOT)) {
            walk(w, 1);
            return;
        }
    }
}

void kiconf_track_ignore(GtkWidget *w)
{
    g_object_set_data(G_OBJECT(w), KEY_IGNORE, GINT_TO_POINTER(1));
}

void kiconf_track_state(GtkWidget *w, KiconfStateFn fn, gpointer data)
{
    g_object_set_data(G_OBJECT(w), KEY_STATE_FN, (gpointer)fn);
    g_object_set_data(G_OBJECT(w), KEY_STATE_DATA, data);
}

void kiconf_track_start(GtkWidget *root)
{
    g_object_set_data(G_OBJECT(root), KEY_ROOT, GINT_TO_POINTER(1));
    walk(root, 1);
}

int kiconf_track_pending(GtkWidget *root)
{
    return walk(root, 0);
}
