/* kipad - tabbed text editor for KiDesktop, GTK2.
 *
 * One process per window. Each tab is a GtkTextView in a scrolled window;
 * its Doc (file name, undo history) hangs off the scrolled window. Settings
 * (font, line numbers) live in ~/.config/kipad.conf and are written whenever
 * they change. */
#include "../shared/xis_i18n.h"
#include "../shared/xis_rootexec.h"
#include "../shared/xis_safefile.h"
#include "../shared/xis_winstate.h"
#include "syntax.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KIPAD_VERSION "0.3.3"
#define DEFAULT_FONT "Monospace 11"
#define MIN_FONT_PT 6
#define MAX_FONT_PT 72
#define UNDO_LIMIT 1000

/* ---- settings -------------------------------------------------------- */

static char g_font[128] = DEFAULT_FONT;
static gboolean g_line_numbers = TRUE;
static gboolean g_word_wrap = FALSE;
/* Files opened while a kipad is running go to a tab of that window. */
static gboolean g_single = TRUE;
/* Seconds between backups of unsaved files; 0 turns the backup off. */
static int g_autosave_secs = 20;
/* Off: no syntax module stays loaded. */
static gboolean g_syntax = TRUE;
/* The Tab key types a tab character, or this many columns of spaces. */
static gboolean g_use_spaces = FALSE;
static int g_tab_spaces = 4;

static char *conf_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kipad.conf", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "kipad.conf", NULL);
}

static void config_load(void)
{
    char *path = conf_path();
    char *data = xis_read_file_or_backup(path, xis_check_usable, NULL);
    if (data) {
        char **lines = g_strsplit(data, "\n", -1);
        for (char **l = lines; *l; l++) {
            char *eq = strchr(*l, '=');
            if (!eq || **l == '#')
                continue;
            *eq = '\0';
            char *key = g_strstrip(*l), *val = g_strstrip(eq + 1);
            if (!strcmp(key, "font") && *val)
                g_strlcpy(g_font, val, sizeof(g_font));
            else if (!strcmp(key, "line_numbers"))
                g_line_numbers = atoi(val) != 0;
            else if (!strcmp(key, "word_wrap"))
                g_word_wrap = atoi(val) != 0;
            else if (!strcmp(key, "single_instance"))
                g_single = atoi(val) != 0;
            else if (!strcmp(key, "autosave_seconds"))
                g_autosave_secs = CLAMP(atoi(val), 0, 3600);
            else if (!strcmp(key, "use_spaces"))
                g_use_spaces = atoi(val) != 0;
            else if (!strcmp(key, "spaces_per_tab"))
                g_tab_spaces = CLAMP(atoi(val), 1, 16);
            else if (!strcmp(key, "syntax_highlighting"))
                g_syntax = atoi(val) != 0;
        }
        g_strfreev(lines);
        free(data);
    }
    g_free(path);
}

static void config_save(void)
{
    char *path = conf_path(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    char *text = g_strdup_printf("# kipad settings, written by kipad\nfont = %s\nline_numbers = %d\nword_wrap = %d\nsingle_instance = %d\nautosave_seconds = %d\n"
                                 "syntax_highlighting = %d\nuse_spaces = %d\nspaces_per_tab = %d\n",
                                 g_font, g_line_numbers, g_word_wrap, g_single, g_autosave_secs, g_syntax, g_use_spaces,
                                 g_tab_spaces);
    xis_write_file(path, text, strlen(text), xis_check_conf_clean);
    g_free(text);
    g_free(dir);
    g_free(path);
}

/* ---- state: cursor positions and open tabs ---------------------------- */

#define POS_LIMIT 200

typedef struct {
    GPtrArray *pos_path;  /* char *, most recently used first */
    GArray *pos_off;      /* int, character offset, parallel to pos_path */
    GPtrArray *tabs;      /* char *, tabs open when the last window closed */
    int active;
} State;

static char *state_path(void)
{
    const char *xdg = g_getenv("XDG_STATE_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kipad", "state", NULL);
    return g_build_filename(g_get_home_dir(), ".local", "state", "kipad", "state", NULL);
}

static void state_init(State *s)
{
    s->pos_path = g_ptr_array_new_with_free_func(g_free);
    s->pos_off = g_array_new(FALSE, FALSE, sizeof(int));
    s->tabs = g_ptr_array_new_with_free_func(g_free);
    s->active = 0;
}

static void state_clear(State *s)
{
    g_ptr_array_free(s->pos_path, TRUE);
    g_array_free(s->pos_off, TRUE);
    g_ptr_array_free(s->tabs, TRUE);
}

static void state_load(State *s)
{
    state_init(s);
    char *path = state_path();
    char *data = xis_read_file_or_backup(path, xis_check_usable, NULL);
    g_free(path);
    if (!data)
        return;
    char **lines = g_strsplit(data, "\n", -1);
    for (char **l = lines; *l; l++) {
        if (g_str_has_prefix(*l, "pos\t")) {
            char *p = *l + 4, *tab = strchr(p, '\t');
            if (!tab || !tab[1])
                continue;
            int off = atoi(p);
            g_ptr_array_add(s->pos_path, g_strdup(tab + 1));
            g_array_append_val(s->pos_off, off);
        } else if (g_str_has_prefix(*l, "tab\t") && (*l)[4]) {
            g_ptr_array_add(s->tabs, g_strdup(*l + 4));
        } else if (g_str_has_prefix(*l, "active\t")) {
            s->active = atoi(*l + 7);
        }
    }
    g_strfreev(lines);
    free(data);
}

static void state_save(const State *s)
{
    GString *out = g_string_new("# kipad state, written by kipad\n");
    for (guint i = 0; i < s->pos_path->len && i < POS_LIMIT; i++)
        g_string_append_printf(out, "pos\t%d\t%s\n", g_array_index(s->pos_off, int, i),
                               (char *)g_ptr_array_index(s->pos_path, i));
    for (guint i = 0; i < s->tabs->len; i++)
        g_string_append_printf(out, "tab\t%s\n", (char *)g_ptr_array_index(s->tabs, i));
    g_string_append_printf(out, "active\t%d\n", s->active);
    char *path = state_path(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    xis_write_file(path, out->str, out->len, NULL);
    g_free(dir);
    g_free(path);
    g_string_free(out, TRUE);
}

static void state_set_pos(State *s, const char *path, int off)
{
    for (guint i = 0; i < s->pos_path->len; i++)
        if (!strcmp(g_ptr_array_index(s->pos_path, i), path)) {
            g_ptr_array_remove_index(s->pos_path, i);
            g_array_remove_index(s->pos_off, i);
            break;
        }
    g_ptr_array_insert(s->pos_path, 0, g_strdup(path));
    g_array_insert_val(s->pos_off, 0, off);
}

static int state_get_pos(const State *s, const char *path)
{
    for (guint i = 0; i < s->pos_path->len; i++)
        if (!strcmp(g_ptr_array_index(s->pos_path, i), path))
            return g_array_index(s->pos_off, int, i);
    return 0;
}

/* ---- documents ------------------------------------------------------- */

typedef struct {
    gboolean insert;
    int offset;      /* in characters */
    char *text;
    gint64 time;     /* for merging typed characters */
} UndoOp;

typedef struct {
    GtkWidget *scroll, *view, *label;
    GtkTextBuffer *buf;
    char *path;       /* NULL: not saved yet */
    int untitled;
    GArray *undo, *redo;   /* UndoOp */
    gboolean replaying;
    int digits;       /* width of the line number column, in digits */
    gboolean bk_dirty;   /* edited since the last backup was written */
    gboolean bk_exists;  /* the hidden backup file is on disk */
    SynDoc *syn;
} Doc;

static GtkWidget *g_win, *g_notebook, *g_status, *g_pos_label, *g_lang_label;
static GtkWidget *g_linenum_item, *g_wrap_item;
static int g_untitled_count;

static Doc *doc_of(GtkWidget *scroll)
{
    return g_object_get_data(G_OBJECT(scroll), "doc");
}

static Doc *doc_current(void)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    return n < 0 ? NULL : doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), n));
}

static char *doc_name(const Doc *d)
{
    if (d->path)
        return g_path_get_basename(d->path);
    return d->untitled > 1 ? g_strdup_printf(_("Untitled %d"), d->untitled) : g_strdup(_("Untitled"));
}

/* ---- backup of unsaved changes --------------------------------------- */

/* "dir/.name.ext.backup": hidden, beside the file. */
static char *backup_path(const char *path)
{
    char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path);
    char *name = g_strdup_printf(".%s.backup", base);
    char *bk = g_build_filename(dir, name, NULL);
    g_free(name);
    g_free(base);
    g_free(dir);
    return bk;
}

static void backup_remove(const char *path)
{
    char *bk = backup_path(path);
    unlink(bk);
    g_free(bk);
}

static void backup_write(Doc *d)
{
    d->bk_dirty = FALSE; /* a failure waits for the next edit instead of retrying */
    if (!d->path)
        return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buf, &a, &b);
    char *text = gtk_text_buffer_get_text(d->buf, &a, &b, TRUE);
    char *bk = backup_path(d->path);
    if (g_file_set_contents(bk, text, -1, NULL))
        d->bk_exists = TRUE;
    g_free(bk);
    g_free(text);
}

static void doc_update_label(Doc *d)
{
    char *name = doc_name(d);
    char *text = gtk_text_buffer_get_modified(d->buf) ? g_strdup_printf("%s *", name) : g_strdup(name);
    gtk_label_set_text(GTK_LABEL(d->label), text);
    if (d->path)
        gtk_widget_set_tooltip_text(d->label, d->path);
    g_free(text);
    g_free(name);
}

static void window_update_title(void)
{
    Doc *d = doc_current();
    if (!d) {
        gtk_window_set_title(GTK_WINDOW(g_win), "kipad");
        return;
    }
    char *name = doc_name(d);
    char *title = g_strdup_printf("%s%s - kipad", gtk_text_buffer_get_modified(d->buf) ? "*" : "", name);
    gtk_window_set_title(GTK_WINDOW(g_win), title);
    g_free(title);
    g_free(name);
}

static void status_update(void)
{
    Doc *d = doc_current();
    const char *lang = d ? syn_doc_language(d->syn) : NULL;
    if (strcmp(gtk_label_get_text(GTK_LABEL(g_lang_label)), lang ? lang : ""))
        gtk_label_set_text(GTK_LABEL(g_lang_label), lang ? lang : "");
    if (!d) {
        gtk_label_set_text(GTK_LABEL(g_pos_label), "");
        return;
    }
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(d->buf, &it, gtk_text_buffer_get_insert(d->buf));
    char *text = g_strdup_printf(_("Line %d, Column %d"), gtk_text_iter_get_line(&it) + 1,
                                 gtk_text_iter_get_line_offset(&it) + 1);
    gtk_label_set_text(GTK_LABEL(g_pos_label), text);
    g_free(text);
}

/* ---- undo ------------------------------------------------------------ */

static void undo_op_clear(gpointer p)
{
    g_free(((UndoOp *)p)->text);
}

static void undo_push(Doc *d, gboolean insert, int offset, const char *text)
{
    gint64 now = g_get_monotonic_time();
    if (d->undo->len) {
        UndoOp *last = &g_array_index(d->undo, UndoOp, d->undo->len - 1);
        /* typing merges into one step while it is contiguous and quick */
        if (insert && last->insert && !strchr(text, '\n') && !strchr(last->text, '\n') &&
            now - last->time < 1000000 && last->offset + (int)g_utf8_strlen(last->text, -1) == offset &&
            g_utf8_strlen(last->text, -1) < 60) {
            char *merged = g_strconcat(last->text, text, NULL);
            g_free(last->text);
            last->text = merged;
            last->time = now;
            g_array_set_size(d->redo, 0);
            return;
        }
    }
    if (d->undo->len >= UNDO_LIMIT)
        g_array_remove_index(d->undo, 0); /* frees the oldest step's text */
    UndoOp op = {insert, offset, g_strdup(text), now};
    g_array_append_val(d->undo, op);
    g_array_set_size(d->redo, 0);
}

static void on_insert_text(GtkTextBuffer *buf, GtkTextIter *at, char *text, int len, Doc *d)
{
    (void)buf;
    if (d->replaying)
        return;
    char *copy = g_strndup(text, (gsize)len);
    undo_push(d, TRUE, gtk_text_iter_get_offset(at), copy);
    g_free(copy);
}

static void on_delete_range(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, Doc *d)
{
    if (d->replaying)
        return;
    char *text = gtk_text_buffer_get_text(buf, start, end, TRUE);
    undo_push(d, FALSE, gtk_text_iter_get_offset(start), text);
    g_free(text);
}

/* Applies `op` (or its inverse when `reverse`) to the buffer. */
static void undo_apply(Doc *d, const UndoOp *op, gboolean reverse)
{
    GtkTextIter a, b;
    gboolean do_insert = op->insert != reverse;
    d->replaying = TRUE;
    gtk_text_buffer_get_iter_at_offset(d->buf, &a, op->offset);
    if (do_insert) {
        gtk_text_buffer_insert(d->buf, &a, op->text, -1);
        gtk_text_buffer_get_iter_at_offset(d->buf, &a, op->offset + (int)g_utf8_strlen(op->text, -1));
    } else {
        gtk_text_buffer_get_iter_at_offset(d->buf, &b, op->offset + (int)g_utf8_strlen(op->text, -1));
        gtk_text_buffer_delete(d->buf, &a, &b);
        gtk_text_buffer_get_iter_at_offset(d->buf, &a, op->offset);
    }
    gtk_text_buffer_place_cursor(d->buf, &a);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(d->view), gtk_text_buffer_get_insert(d->buf));
    d->replaying = FALSE;
}

static void undo_cb(void)
{
    Doc *d = doc_current();
    if (!d || !d->undo->len)
        return;
    UndoOp op = g_array_index(d->undo, UndoOp, d->undo->len - 1);
    op.text = g_strdup(op.text); /* removing the element frees its own copy */
    g_array_remove_index(d->undo, d->undo->len - 1);
    undo_apply(d, &op, TRUE);
    g_array_append_val(d->redo, op); /* ownership of op.text moves to redo */
}

static void redo_cb(void)
{
    Doc *d = doc_current();
    if (!d || !d->redo->len)
        return;
    UndoOp op = g_array_index(d->redo, UndoOp, d->redo->len - 1);
    op.text = g_strdup(op.text);
    g_array_remove_index(d->redo, d->redo->len - 1);
    undo_apply(d, &op, FALSE);
    op.time = 0; /* a redone step never merges with what is typed next */
    g_array_append_val(d->undo, op);
}

/* ---- font and zoom --------------------------------------------------- */

static void apply_font_to(Doc *d)
{
    PangoFontDescription *fd = pango_font_description_from_string(g_font);
    gtk_widget_modify_font(d->view, fd);
    pango_font_description_free(fd);
}

static void linenum_resize(Doc *d);

static void apply_font_all(void)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        Doc *d = doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        apply_font_to(d);
        d->digits = 0; /* the digit width changed */
        linenum_resize(d);
        gtk_widget_queue_draw(d->view);
    }
}

static void zoom(int delta)
{
    PangoFontDescription *fd = pango_font_description_from_string(g_font);
    int pt = pango_font_description_get_size(fd) / PANGO_SCALE;
    if (pt <= 0)
        pt = 11;
    pt = CLAMP(pt + delta, MIN_FONT_PT, MAX_FONT_PT);
    pango_font_description_set_size(fd, pt * PANGO_SCALE);
    char *s = pango_font_description_to_string(fd);
    g_strlcpy(g_font, s, sizeof(g_font));
    g_free(s);
    pango_font_description_free(fd);
    apply_font_all();
    config_save();
}

static void font_cb(void)
{
    GtkWidget *dlg = gtk_font_selection_dialog_new(_("Font"));
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(g_win));
    gtk_font_selection_dialog_set_font_name(GTK_FONT_SELECTION_DIALOG(dlg), g_font);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char *name = gtk_font_selection_dialog_get_font_name(GTK_FONT_SELECTION_DIALOG(dlg));
        if (name && *name) {
            g_strlcpy(g_font, name, sizeof(g_font));
            apply_font_all();
            config_save();
        }
        g_free(name);
    }
    gtk_widget_destroy(dlg);
}

/* Tab types spaces up to the next multiple of the width when the option is
 * on; Enter starts the new line with the previous line's indentation. */
static gboolean on_view_key(GtkWidget *w, GdkEventKey *ev, Doc *d)
{
    (void)w;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    gboolean tab = ev->keyval == GDK_Tab && !mods;
    gboolean enter = (ev->keyval == GDK_Return || ev->keyval == GDK_KP_Enter) && !(mods & ~GDK_SHIFT_MASK);
    if ((tab && !g_use_spaces) || (!tab && !enter))
        return FALSE;
    GtkTextBuffer *buf = d->buf;
    gtk_text_buffer_begin_user_action(buf);
    gtk_text_buffer_delete_selection(buf, TRUE, TRUE);
    GtkTextIter cur, ls;
    gtk_text_buffer_get_iter_at_mark(buf, &cur, gtk_text_buffer_get_insert(buf));
    ls = cur;
    gtk_text_iter_set_line_offset(&ls, 0);
    GString *out = g_string_new(NULL);
    if (tab) {
        int col = gtk_text_iter_get_line_offset(&cur);
        g_string_append_len(out, "                ", g_tab_spaces - col % g_tab_spaces);
    } else {
        g_string_append_c(out, '\n');
        GtkTextIter p = ls;
        while (gtk_text_iter_compare(&p, &cur) < 0) {
            gunichar c = gtk_text_iter_get_char(&p);
            if (c != ' ' && c != '\t')
                break;
            g_string_append_c(out, (char)c);
            gtk_text_iter_forward_char(&p);
        }
    }
    gtk_text_buffer_insert_interactive_at_cursor(buf, out->str, (gint)out->len, gtk_text_view_get_editable(GTK_TEXT_VIEW(d->view)));
    gtk_text_buffer_end_user_action(buf);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(d->view), gtk_text_buffer_get_insert(buf));
    g_string_free(out, TRUE);
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)data;
    if ((ev->state & GDK_SHIFT_MASK) && !(ev->state & GDK_CONTROL_MASK)) {
        /* shift + wheel scrolls sideways */
        GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(gtk_widget_get_parent(w)));
        double step = gtk_adjustment_get_step_increment(adj), v = gtk_adjustment_get_value(adj);
        if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT)
            v -= step * 3;
        else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT)
            v += step * 3;
        else
            return FALSE;
        gtk_adjustment_set_value(adj, CLAMP(v, gtk_adjustment_get_lower(adj), gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)));
        return TRUE;
    }
    if (!(ev->state & GDK_CONTROL_MASK))
        return FALSE;
    if (ev->direction == GDK_SCROLL_UP)
        zoom(1);
    else if (ev->direction == GDK_SCROLL_DOWN)
        zoom(-1);
    return TRUE;
}

/* ---- line numbers ---------------------------------------------------- */

static int digits_of(int n)
{
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

/* Sizes the left border window for the current line count. */
static void linenum_resize(Doc *d)
{
    GtkTextView *tv = GTK_TEXT_VIEW(d->view);
    if (!g_line_numbers) {
        gtk_text_view_set_border_window_size(tv, GTK_TEXT_WINDOW_LEFT, 0);
        d->digits = 0;
        return;
    }
    int digits = MAX(digits_of(gtk_text_buffer_get_line_count(d->buf)), 3);
    if (digits == d->digits)
        return;
    d->digits = digits;
    PangoLayout *l = gtk_widget_create_pango_layout(d->view, "0");
    int w;
    pango_layout_get_pixel_size(l, &w, NULL);
    g_object_unref(l);
    gtk_text_view_set_border_window_size(tv, GTK_TEXT_WINDOW_LEFT, w * digits + 10);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, Doc *d)
{
    GtkTextView *tv = GTK_TEXT_VIEW(w);
    GdkWindow *win = gtk_text_view_get_window(tv, GTK_TEXT_WINDOW_LEFT);
    if (!g_line_numbers || ev->window != win)
        return FALSE;

    int by1, by2, top;
    gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_LEFT, 0, ev->area.y, NULL, &by1);
    gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_LEFT, 0, ev->area.y + ev->area.height, NULL, &by2);

    GtkTextIter it, cursor;
    gtk_text_buffer_get_iter_at_mark(d->buf, &cursor, gtk_text_buffer_get_insert(d->buf));
    int cur_line = gtk_text_iter_get_line(&cursor);
    gtk_text_view_get_line_at_y(tv, &it, by1, &top);
    int first = gtk_text_iter_get_line(&it), count = gtk_text_buffer_get_line_count(d->buf);

    int width = gdk_window_get_width(win);
    PangoLayout *l = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_width(l, (width - 6) * PANGO_SCALE);
    pango_layout_set_alignment(l, PANGO_ALIGN_RIGHT);
    /* by line index: forward_line() stops short of an empty last line */
    for (int line = first; line < count; line++) {
        int y, h, wy;
        gtk_text_buffer_get_iter_at_line(d->buf, &it, line);
        gtk_text_view_get_line_yrange(tv, &it, &y, &h);
        if (y > by2)
            break;
        gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_LEFT, 0, y, NULL, &wy);
        char num[16];
        snprintf(num, sizeof(num), "%d", line + 1);
        pango_layout_set_text(l, num, -1);
        gtk_paint_layout(gtk_widget_get_style(w), win, line == cur_line ? GTK_STATE_NORMAL : GTK_STATE_INSENSITIVE,
                         FALSE, &ev->area, w, NULL, 0, wy, l);
    }
    g_object_unref(l);
    return FALSE;
}

static void set_line_numbers(gboolean on)
{
    g_line_numbers = on;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        Doc *d = doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        d->digits = 0;
        linenum_resize(d);
    }
    config_save();
}

static void linenum_toggle_cb(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    if (gtk_check_menu_item_get_active(item) != g_line_numbers)
        set_line_numbers(gtk_check_menu_item_get_active(item));
}

static void apply_wrap_to(Doc *d)
{
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->view), g_word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
}

static void wrap_toggle_cb(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    gboolean on = gtk_check_menu_item_get_active(item);
    if (on == g_word_wrap)
        return;
    g_word_wrap = on;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++)
        apply_wrap_to(doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i)));
    config_save();
}

/* ---- buffer signals -------------------------------------------------- */

static void on_changed(GtkTextBuffer *buf, Doc *d)
{
    (void)buf;
    d->bk_dirty = TRUE;
    linenum_resize(d);
    gtk_widget_queue_draw(d->view);
    if (d == doc_current())
        status_update(); /* typing moves the cursor without a mark-set */
}

static void on_modified(GtkTextBuffer *buf, Doc *d)
{
    if (!gtk_text_buffer_get_modified(buf)) {
        /* saved, reloaded or discarded: nothing left to recover */
        d->bk_dirty = FALSE;
        d->bk_exists = FALSE;
        if (d->path)
            backup_remove(d->path);
    }
    doc_update_label(d);
    if (d == doc_current())
        window_update_title();
}

static void on_mark_set(GtkTextBuffer *buf, GtkTextIter *loc, GtkTextMark *mark, Doc *d)
{
    (void)buf;
    (void)loc;
    if (mark != gtk_text_buffer_get_insert(d->buf))
        return;
    if (d == doc_current())
        status_update();
    if (g_line_numbers)
        gtk_widget_queue_draw(d->view); /* the current line's number is highlighted */
}

/* ---- tabs ------------------------------------------------------------ */

static void close_tab(GtkWidget *scroll);

static Doc *g_find_hl;
static void doc_free(Doc *d)
{
    if (g_find_hl == d)
        g_find_hl = NULL;
    syn_doc_free(d->syn);
    g_array_free(d->undo, TRUE);
    g_array_free(d->redo, TRUE);
    g_free(d->path);
    g_free(d);
}

static void tab_menu_popup(GtkWidget *scroll, GdkEventButton *ev);

static gboolean tab_button_press(GtkWidget *w, GdkEventButton *ev, GtkWidget *scroll)
{
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    tab_menu_popup(scroll, ev);
    return TRUE;
}

static void tab_close_clicked(GtkWidget *btn, GtkWidget *scroll)
{
    (void)btn;
    close_tab(scroll);
}

static Doc *doc_new(void)
{
    Doc *d = g_new0(Doc, 1);
    d->undo = g_array_new(FALSE, TRUE, sizeof(UndoOp));
    d->redo = g_array_new(FALSE, TRUE, sizeof(UndoOp));
    g_array_set_clear_func(d->undo, undo_op_clear);
    g_array_set_clear_func(d->redo, undo_op_clear);

    d->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(d->scroll, "kipad-view-sw");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(d->scroll), GTK_SHADOW_IN);
    d->view = gtk_text_view_new();
    d->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->view));
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d->view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(d->view), 4);
    gtk_container_add(GTK_CONTAINER(d->scroll), d->view);
    d->syn = syn_doc_new(d->scroll, GTK_TEXT_VIEW(d->view));
    g_object_set_data_full(G_OBJECT(d->scroll), "doc", d, (GDestroyNotify)doc_free);
    apply_font_to(d);
    apply_wrap_to(d);
    linenum_resize(d);

    gtk_text_buffer_create_tag(d->buf, "find-match", "background", "#ffd54f", "foreground", "#000000", NULL);
    g_signal_connect(d->buf, "insert-text", G_CALLBACK(on_insert_text), d);
    g_signal_connect(d->buf, "delete-range", G_CALLBACK(on_delete_range), d);
    g_signal_connect(d->buf, "changed", G_CALLBACK(on_changed), d);
    g_signal_connect(d->buf, "modified-changed", G_CALLBACK(on_modified), d);
    g_signal_connect(d->buf, "mark-set", G_CALLBACK(on_mark_set), d);
    g_signal_connect(d->view, "expose-event", G_CALLBACK(on_expose), d);
    g_signal_connect(d->view, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(d->view, "key-press-event", G_CALLBACK(on_view_key), d);

    /* tab label: name + close button */
    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    d->label = gtk_label_new("");
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
    gtk_container_add(GTK_CONTAINER(btn), gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(btn, _("Close tab"));
    g_signal_connect(btn, "clicked", G_CALLBACK(tab_close_clicked), d->scroll);
    gtk_box_pack_start(GTK_BOX(box), d->label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    GtkWidget *ebox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
    gtk_container_add(GTK_CONTAINER(ebox), box);
    g_signal_connect(ebox, "button-press-event", G_CALLBACK(tab_button_press), d->scroll);
    gtk_widget_show_all(ebox);

    gtk_widget_show_all(d->scroll);
    int n = gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), d->scroll, ebox);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(g_notebook), d->scroll, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), n);
    gtk_widget_grab_focus(d->view);
    return d;
}

static Doc *new_empty_tab(void)
{
    Doc *d = doc_new();
    d->untitled = ++g_untitled_count;
    doc_update_label(d);
    window_update_title();
    return d;
}

static void doc_set_text(Doc *d, const char *text, gsize len)
{
    d->replaying = TRUE;
    gtk_text_buffer_set_text(d->buf, text, (gint)len);
    d->replaying = FALSE;
    g_array_set_size(d->undo, 0);
    g_array_set_size(d->redo, 0);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(d->buf, &start);
    gtk_text_buffer_place_cursor(d->buf, &start);
    gtk_text_buffer_set_modified(d->buf, FALSE);
}

static int doc_cursor_offset(const Doc *d)
{
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(d->buf, &it, gtk_text_buffer_get_insert(d->buf));
    return gtk_text_iter_get_offset(&it);
}

static void doc_remember_cursor(const Doc *d)
{
    if (!d->path)
        return;
    State s;
    state_load(&s);
    state_set_pos(&s, d->path, doc_cursor_offset(d));
    state_save(&s);
    state_clear(&s);
}

static gboolean scroll_to_cursor_idle(gpointer data)
{
    Doc *d = data;
    GtkTextMark *m = gtk_text_buffer_get_insert(d->buf);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(d->view), m, 0.0, TRUE, 0.0, 0.5);
    g_object_unref(d->scroll);
    return FALSE;
}

static void doc_restore_cursor(Doc *d, int off)
{
    if (off <= 0)
        return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_offset(d->buf, &it, off); /* clamps to the end */
    gtk_text_buffer_place_cursor(d->buf, &it);
    /* the view has no size yet: scroll once the layout exists */
    g_object_ref(d->scroll);
    g_idle_add(scroll_to_cursor_idle, d);
}

static void message(GtkMessageType type, const char *fmt, const char *arg)
{
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, fmt, arg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* The file's text as UTF-8, or NULL after telling the user why not. */
static char *read_utf8_file(const char *abs, gsize *len)
{
    char *data = NULL;
    GError *err = NULL;
    if (!g_file_get_contents(abs, &data, len, &err)) {
        message(GTK_MESSAGE_ERROR, _("Could not open the file:\n%s"), err->message);
        g_error_free(err);
        return NULL;
    }
    if (g_utf8_validate(data, (gssize)*len, NULL))
        return data;
    /* not UTF-8: assume the usual legacy encoding */
    char *utf8 = g_convert(data, (gssize)*len, "UTF-8", "ISO-8859-1", NULL, len, NULL);
    g_free(data);
    if (!utf8)
        message(GTK_MESSAGE_ERROR, _("Could not read the file:\n%s"), abs);
    return utf8;
}

/* ---- autosave -------------------------------------------------------- */

static void state_remember_tabs(gboolean with_positions);

static gboolean autosave_tick(gpointer data)
{
    (void)data;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        Doc *d = doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        if (!d->path || !d->bk_dirty || !gtk_text_buffer_get_modified(d->buf))
            continue;
        gboolean fresh = !d->bk_exists;
        backup_write(d);
        /* a crash must find this file in the tab list even if it was opened
         * after the last clean exit */
        if (fresh && d->bk_exists)
            state_remember_tabs(FALSE);
    }
    return TRUE;
}

static guint g_autosave_src;

static void autosave_restart(void)
{
    if (g_autosave_src)
        g_source_remove(g_autosave_src);
    g_autosave_src = g_autosave_secs > 0 ? g_timeout_add_seconds((guint)g_autosave_secs, autosave_tick, NULL) : 0;
}

static char *file_time(const char *path)
{
    struct stat st;
    char buf[64] = { 0 };
    if (stat(path, &st) == 0) {
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(buf, sizeof buf, "%x %X", &tm);
    }
    return g_strdup(buf);
}

enum { BK_CANCEL, BK_SAVED, BK_BACKUP };

/* A hidden backup exists beside `abs`: which one to open? Identical copies
 * need no question. */
static int backup_ask(const char *abs, const char *bk, const char *saved, gsize saved_len, const char *bktext,
                      gsize bk_len)
{
    if (saved_len == bk_len && !memcmp(saved, bktext, bk_len))
        return BK_SAVED;
    char *name = g_path_get_basename(abs), *t_saved = file_time(abs), *t_bk = file_time(bk);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                            _("A backup with unsaved changes of \"%s\" was found."), name);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dlg),
        _("Saved file: %s\nBackup: %s\n\nOpening the saved file discards the backup."), t_saved, t_bk);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("Open saved file"),
                           GTK_RESPONSE_REJECT, _("Open backup"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(name);
    g_free(t_saved);
    g_free(t_bk);
    return r == GTK_RESPONSE_ACCEPT ? BK_BACKUP : r == GTK_RESPONSE_REJECT ? BK_SAVED : BK_CANCEL;
}

static void open_path(const char *path);

/* ---- recent files (freedesktop recently-used.xbel) ------------------- */

#define RECENT_MAX 10

static void recent_add(const char *path)
{
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!uri)
        return;
    char *ct = g_content_type_guess(path, NULL, 0, NULL);
    char *mime = ct ? g_content_type_get_mime_type(ct) : NULL;
    GtkRecentData rd = { 0 };
    rd.mime_type = mime ? mime : (char *)"text/plain";
    rd.app_name = (char *)"kipad";
    rd.app_exec = (char *)"kipad %u";
    gtk_recent_manager_add_full(gtk_recent_manager_get_default(), uri, &rd);
    g_free(mime);
    g_free(ct);
    g_free(uri);
}

static gint recent_newest_first(gconstpointer a, gconstpointer b)
{
    time_t ta = gtk_recent_info_get_modified((GtkRecentInfo *)a), tb = gtk_recent_info_get_modified((GtkRecentInfo *)b);
    return (tb > ta) - (tb < ta);
}

static void recent_item_cb(GtkWidget *it, gpointer data)
{
    (void)data;
    open_path(g_object_get_data(G_OBJECT(it), "kipad-path"));
}

/* Rebuilt every time the submenu opens, so other programs' changes show up. */
static void recent_menu_fill(GtkWidget *menu, gpointer data)
{
    (void)data;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *k = kids; k; k = k->next)
        gtk_widget_destroy(k->data);
    g_list_free(kids);

    GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
    items = g_list_sort(items, recent_newest_first);
    int shown = 0;
    for (GList *l = items; l && shown < RECENT_MAX; l = l->next) {
        GtkRecentInfo *ri = l->data;
        if (!gtk_recent_info_has_application(ri, "kipad"))
            continue;
        char *path = g_filename_from_uri(gtk_recent_info_get_uri(ri), NULL, NULL);
        if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            g_free(path);
            continue;
        }
        char *base = g_path_get_basename(path);
        GtkWidget *it = gtk_menu_item_new_with_label(base);
        gtk_widget_set_tooltip_text(it, path);
        g_object_set_data_full(G_OBJECT(it), "kipad-path", path, g_free);
        g_signal_connect(it, "activate", G_CALLBACK(recent_item_cb), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        g_free(base);
        shown++;
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    if (!shown) {
        GtkWidget *it = gtk_menu_item_new_with_label(_("(none)"));
        gtk_widget_set_sensitive(it, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    gtk_widget_show_all(menu);
}

/* Opens `path` in a tab (an already open one is just shown). */
static void open_path(const char *path)
{
    char *cwd = g_get_current_dir();
    char *abs = g_path_is_absolute(path) ? g_strdup(path) : g_build_filename(cwd, path, NULL);
    g_free(cwd);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        Doc *d = doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        if (d->path && !strcmp(d->path, abs)) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), i);
            g_free(abs);
            return;
        }
    }
    gsize len = 0;
    char *utf8 = read_utf8_file(abs, &len);
    if (!utf8) {
        g_free(abs);
        return;
    }
    int choice = BK_SAVED;
    char *bk = backup_path(abs);
    if (g_file_test(bk, G_FILE_TEST_IS_REGULAR)) {
        gsize bklen = 0;
        char *bktext = read_utf8_file(bk, &bklen);
        if (bktext) {
            choice = backup_ask(abs, bk, utf8, len, bktext, bklen);
            if (choice == BK_BACKUP) {
                g_free(utf8);
                utf8 = bktext;
                len = bklen;
            } else
                g_free(bktext);
        }
    }
    g_free(bk);
    if (choice == BK_CANCEL) {
        g_free(utf8);
        g_free(abs);
        return;
    }
    /* an untouched empty tab is reused instead of left behind */
    Doc *cur = doc_current();
    Doc *d = (cur && !cur->path && !gtk_text_buffer_get_modified(cur->buf) &&
              gtk_text_buffer_get_char_count(cur->buf) == 0)
                 ? cur
                 : doc_new();
    d->path = abs;
    doc_set_text(d, utf8, len); /* also drops the backup file */
    g_free(utf8);
    if (choice == BK_BACKUP) {
        /* the unsaved text, shown as the saved file with changes */
        gtk_text_buffer_set_modified(d->buf, TRUE);
        backup_write(d);
    }
    syn_doc_set_file(d->syn, d->path);
    recent_add(d->path);
    State st;
    state_load(&st);
    doc_restore_cursor(d, state_get_pos(&st, d->path));
    state_clear(&st);
    doc_update_label(d);
    window_update_title();
    status_update();
}

static gboolean save_doc_as(Doc *d);

static gboolean save_doc(Doc *d)
{
    if (!d->path)
        return save_doc_as(d);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buf, &a, &b);
    char *text = gtk_text_buffer_get_text(d->buf, &a, &b, TRUE);
    /* The user's own document: atomic and synced, but no .backup beside it. */
    gboolean ok = xis_write_export(d->path, text, strlen(text)) == 0;
    int werr = errno;
    /* No permission (a root-owned file): ask for the password, every time. */
    if (!ok && (werr == EACCES || werr == EPERM)) {
        char *const argv[] = { "tee", "--", d->path, NULL };
        int rc = xis_rootexec(argv, text, strlen(text));
        if (rc == 0)
            ok = TRUE;
        else if (rc == XIS_ROOTEXEC_CANCELLED) {
            g_free(text);
            return FALSE;
        } else
            werr = 0;
    }
    g_free(text);
    if (!ok) {
        message(GTK_MESSAGE_ERROR, _("Could not save the file:\n%s"),
                werr ? g_strerror(werr) : _("The administrator command failed."));
        return FALSE;
    }
    gtk_text_buffer_set_modified(d->buf, FALSE);
    doc_update_label(d);
    window_update_title();
    return TRUE;
}

static gboolean save_doc_as(Doc *d)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Save as"), GTK_WINDOW(g_win), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_SAVE,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (d->path)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), d->path);
    gboolean ok = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *old = d->path;
        d->path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        ok = save_doc(d);
        if (ok) {
            if (old && strcmp(old, d->path))
                backup_remove(old);
            g_free(old);
            syn_doc_set_file(d->syn, d->path);
            status_update();
        } else {
            g_free(d->path);
            d->path = old;
        }
    }
    gtk_widget_destroy(dlg);
    return ok;
}

/* TRUE when it is fine to discard `d` (saved, or the user said so). */
static gboolean confirm_discard(Doc *d)
{
    if (!gtk_text_buffer_get_modified(d->buf))
        return TRUE;
    char *name = doc_name(d);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                            _("Save changes to \"%s\" before closing?"), name);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), _("Close without saving"), GTK_RESPONSE_REJECT, GTK_STOCK_CANCEL,
                           GTK_RESPONSE_CANCEL, GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(name);
    if (r == GTK_RESPONSE_REJECT)
        return TRUE;
    return r == GTK_RESPONSE_ACCEPT && save_doc(d);
}

static void close_tab(GtkWidget *scroll)
{
    Doc *d = doc_of(scroll);
    int n = gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), scroll);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), n);
    if (!confirm_discard(d))
        return;
    if (d->path)
        backup_remove(d->path); /* closed on purpose: the changes are gone */
    doc_remember_cursor(d);
    gtk_notebook_remove_page(GTK_NOTEBOOK(g_notebook), n);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 0) {
        /* closed the last tab on purpose: next start is empty */
        State s;
        state_load(&s);
        g_ptr_array_set_size(s.tabs, 0);
        state_save(&s);
        state_clear(&s);
        gtk_main_quit();
    }
}

/* Remembers the tab list (and, on a clean exit, every cursor) to reopen them
 * next time. */
static void state_remember_tabs(gboolean with_positions)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    State s;
    state_load(&s);
    g_ptr_array_set_size(s.tabs, 0);
    int active = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    s.active = 0;
    for (int i = 0; i < n; i++) {
        Doc *d = doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        if (!d->path)
            continue;
        if (with_positions)
            state_set_pos(&s, d->path, doc_cursor_offset(d));
        if (i == active)
            s.active = (int)s.tabs->len;
        g_ptr_array_add(s.tabs, g_strdup(d->path));
    }
    state_save(&s);
    state_clear(&s);
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    /* The prompts walk through the tabs; the one open at close is what gets reopened. */
    int shown = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), i);
        if (!confirm_discard(doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i)))) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), shown);
            return TRUE;
        }
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), shown);
    for (int i = 0; i < n; i++) {
        Doc *d = doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i));
        if (d->path)
            backup_remove(d->path);
    }
    state_remember_tabs(TRUE);
    return FALSE;
}

/* ---- tab context menu ------------------------------------------------ */

/* TRUE when it is fine to lose `d`'s unsaved changes (nothing to lose, or the
 * user agreed). */
static gboolean confirm_lose_changes(Doc *d, const char *what)
{
    if (!gtk_text_buffer_get_modified(d->buf))
        return TRUE;
    char *name = doc_name(d);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                            _("\"%s\" has unsaved changes."), name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", what);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("Discard changes"),
                           GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(name);
    return r == GTK_RESPONSE_ACCEPT;
}

static void tab_close_item(GtkWidget *item, GtkWidget *scroll)
{
    (void)item;
    close_tab(scroll);
}

/* Replaces the text with the file on disk (unsaved changes and their backup
 * are dropped). */
static void doc_reload(Doc *d, int cursor)
{
    gsize len = 0;
    char *utf8 = read_utf8_file(d->path, &len);
    if (!utf8)
        return;
    doc_set_text(d, utf8, len);
    g_free(utf8);
    doc_restore_cursor(d, cursor);
    doc_update_label(d);
    window_update_title();
    status_update();
}

static void tab_reopen_item(GtkWidget *item, GtkWidget *scroll)
{
    (void)item;
    Doc *d = doc_of(scroll);
    if (!d->path || !confirm_lose_changes(d, _("Reopening reloads the file from disk and the changes will be lost.")))
        return;
    doc_reload(d, 0);
}

/* Opens the tab's file in a window of its own (--new-window skips the
 * single-instance hand-over) and closes the tab here. */
static void tab_detach_item(GtkWidget *item, GtkWidget *scroll)
{
    (void)item;
    Doc *d = doc_of(scroll);
    if (!d->path || !confirm_lose_changes(d, _("The file is reopened from disk in another window and the changes will be lost.")))
        return;
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *argv[] = { exe ? exe : (char *)"kipad", (char *)"--new-window", (char *)"--", d->path, NULL };
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        message(GTK_MESSAGE_ERROR, _("Could not open a new window:\n%s"), err->message);
        g_error_free(err);
        g_free(exe);
        return;
    }
    g_free(exe);
    gtk_text_buffer_set_modified(d->buf, FALSE); /* decided above: no second prompt */
    close_tab(scroll);
}

static void tab_copy_path_item(GtkWidget *item, GtkWidget *scroll)
{
    (void)item;
    Doc *d = doc_of(scroll);
    if (!d->path)
        return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), d->path, -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), d->path, -1);
}

static void tab_menu_popup(GtkWidget *scroll, GdkEventButton *ev)
{
    Doc *d = doc_of(scroll);
    GtkWidget *menu = gtk_menu_new();
    struct {
        const char *label;
        GCallback cb;
        gboolean needs_path;
    } items[] = {
        { N_("Close"), G_CALLBACK(tab_close_item), FALSE },
        { N_("Reopen"), G_CALLBACK(tab_reopen_item), TRUE },
        { N_("Detach"), G_CALLBACK(tab_detach_item), TRUE },
        { N_("Copy path"), G_CALLBACK(tab_copy_path_item), TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *it = gtk_menu_item_new_with_label(_(items[i].label));
        gtk_widget_set_sensitive(it, !items[i].needs_path || d->path != NULL);
        g_signal_connect(it, "activate", items[i].cb, scroll);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    gtk_widget_show_all(menu);
    /* the menu goes away with its selection */
    g_signal_connect_swapped(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
}

/* ---- single instance -------------------------------------------------- */

/* The first kipad of a display listens on a Unix socket; a later one hands it
 * its files (NUL-separated absolute paths) and exits. A leftover socket from a
 * crash is told apart by the refused connection. */
static GSocketService *g_service;
static char *g_sock_path;

static char *sock_path(void)
{
    const char *rt = g_getenv("XDG_RUNTIME_DIR"), *disp = g_getenv("DISPLAY");
    char *name = g_strdup_printf("kipad%s.sock", disp ? disp : "");
    for (char *p = name; *p; p++)
        if (*p == ':' || *p == '/')
            *p = '_';
    char *path = g_build_filename(rt && *rt ? rt : g_get_tmp_dir(), name, NULL);
    g_free(name);
    return path;
}

static void open_paths_blob(const char *buf, gsize len)
{
    for (gsize i = 0; i < len;) {
        gsize n = strnlen(buf + i, len - i);
        if (n)
            open_path(buf + i);
        i += n + 1;
    }
}

static gboolean on_service_incoming(GSocketService *svc, GSocketConnection *conn, GObject *src, gpointer data)
{
    (void)svc;
    (void)src;
    (void)data;
    g_socket_set_timeout(g_socket_connection_get_socket(conn), 2);
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GString *blob = g_string_new(NULL);
    char buf[4096];
    gssize n;
    while (blob->len < (1 << 20) && (n = g_input_stream_read(in, buf, sizeof(buf), NULL, NULL)) > 0)
        g_string_append_len(blob, buf, n);
    open_paths_blob(blob->str, blob->len);
    g_string_free(blob, TRUE);
    gtk_window_deiconify(GTK_WINDOW(g_win));
    gtk_window_present(GTK_WINDOW(g_win));
    return TRUE;
}

/* TRUE when a running kipad took the files. */
static gboolean handover_to_running(char **paths, int n)
{
    char *path = sock_path();
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 2);
    GSocketAddress *addr = g_unix_socket_address_new(path);
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    gboolean ok = conn != NULL;
    if (conn) {
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        char *cwd = g_get_current_dir();
        for (int i = 0; i < n && ok; i++) {
            char *abs = g_path_is_absolute(paths[i]) ? g_strdup(paths[i]) : g_build_filename(cwd, paths[i], NULL);
            ok = g_output_stream_write_all(out, abs, strlen(abs) + 1, NULL, NULL, NULL);
            g_free(abs);
        }
        g_free(cwd);
        g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
        g_object_unref(conn);
    }
    g_object_unref(addr);
    g_object_unref(client);
    g_free(path);
    return ok;
}

static void service_start(void)
{
    if (g_service)
        return;
    char *path = sock_path();
    for (int attempt = 0; attempt < 2; attempt++) {
        GSocketService *svc = g_socket_service_new();
        GSocketAddress *addr = g_unix_socket_address_new(path);
        GError *err = NULL;
        gboolean ok = g_socket_listener_add_address(G_SOCKET_LISTENER(svc), addr, G_SOCKET_TYPE_STREAM,
                                                    G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err);
        g_object_unref(addr);
        if (ok) {
            chmod(path, 0600);
            g_signal_connect(svc, "incoming", G_CALLBACK(on_service_incoming), NULL);
            g_socket_service_start(svc);
            g_service = svc;
            g_sock_path = path;
            return;
        }
        g_clear_error(&err);
        g_object_unref(svc);
        /* Address in use: stale file from a crash (nobody answers), since the
         * live case was handled by the hand-over before. */
        unlink(path);
    }
    g_free(path);
}

static void service_stop(void)
{
    if (!g_service)
        return;
    g_socket_service_stop(g_service);
    g_socket_listener_close(G_SOCKET_LISTENER(g_service));
    g_object_unref(g_service);
    g_service = NULL;
    unlink(g_sock_path);
    g_clear_pointer(&g_sock_path, g_free);
}

static void autosave_cb(void)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Autosave"), GTK_WINDOW(g_win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_hbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 3600, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), g_autosave_secs);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Back up unsaved files every (seconds, 0 = off):")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), box, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(box);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        g_autosave_secs = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
        config_save();
        autosave_restart();
    }
    gtk_widget_destroy(dlg);
}

static void tabs_cb(void)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Tab key"), GTK_WINDOW(g_win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *vb = gtk_vbox_new(FALSE, 8), *hb = gtk_hbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 8);
    GtkWidget *chk = gtk_check_button_new_with_label(_("Type spaces instead of a tab character"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_use_spaces);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), g_tab_spaces);
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(hb), gtk_label_new(_("Spaces per tab:")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), hb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), vb, FALSE, FALSE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(vb);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        g_use_spaces = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk));
        g_tab_spaces = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
        config_save();
    }
    gtk_widget_destroy(dlg);
}

static void syntax_toggle_cb(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    g_syntax = gtk_check_menu_item_get_active(item);
    config_save();
    syn_set_enabled(g_syntax);
}

static void single_toggle_cb(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    g_single = gtk_check_menu_item_get_active(item);
    config_save();
    if (g_single)
        service_start();
    else
        service_stop();
}

/* ---- menu actions ---------------------------------------------------- */

static void new_cb(void)
{
    new_empty_tab();
}

static void open_cb(void)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Open"), GTK_WINDOW(g_win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OPEN,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList *l = files; l; l = l->next) {
            open_path(l->data);
            g_free(l->data);
        }
        g_slist_free(files);
    }
    gtk_widget_destroy(dlg);
}

static void save_cb(void)
{
    Doc *d = doc_current();
    if (d)
        save_doc(d);
}

static void save_as_cb(void)
{
    Doc *d = doc_current();
    if (d)
        save_doc_as(d);
}

static void close_cb(void)
{
    Doc *d = doc_current();
    if (d)
        close_tab(d->scroll);
}

/* Close (asking to save) and open again, in the same tab. */
static void reopen_cb(void)
{
    Doc *d = doc_current();
    if (!d || !d->path || !confirm_discard(d))
        return;
    doc_reload(d, doc_cursor_offset(d));
}

static void quit_cb(void)
{
    if (!on_delete_event(NULL, NULL, NULL))
        gtk_main_quit();
}

static void cut_cb(void)
{
    Doc *d = doc_current();
    if (d)
        gtk_text_buffer_cut_clipboard(d->buf, gtk_widget_get_clipboard(d->view, GDK_SELECTION_CLIPBOARD), TRUE);
}

static void copy_cb(void)
{
    Doc *d = doc_current();
    if (d)
        gtk_text_buffer_copy_clipboard(d->buf, gtk_widget_get_clipboard(d->view, GDK_SELECTION_CLIPBOARD));
}

static void paste_cb(void)
{
    Doc *d = doc_current();
    if (d)
        gtk_text_buffer_paste_clipboard(d->buf, gtk_widget_get_clipboard(d->view, GDK_SELECTION_CLIPBOARD), NULL, TRUE);
}

static void select_all_cb(void)
{
    Doc *d = doc_current();
    if (!d)
        return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buf, &a, &b);
    gtk_text_buffer_select_range(d->buf, &a, &b);
}

static void zoom_in_cb(void)
{
    zoom(1);
}

static void zoom_out_cb(void)
{
    zoom(-1);
}

static void zoom_reset_cb(void)
{
    g_strlcpy(g_font, DEFAULT_FONT, sizeof(g_font));
    apply_font_all();
    config_save();
}

static void about_cb(void)
{
    gtk_show_about_dialog(GTK_WINDOW(g_win), "program-name", "kipad", "version", KIPAD_VERSION, "comments",
                          _("A simple tabbed text editor"), "logo-icon-name", "kipad", NULL);
}

static gboolean page_switched_idle(gpointer data)
{
    (void)data;
    window_update_title();
    status_update();
    return FALSE;
}

/* "switch-page" runs before the notebook's current page changes. */
static void page_switched(GtkNotebook *nb, gpointer page, guint n, gpointer data)
{
    (void)nb;
    (void)page;
    (void)n;
    (void)data;
    g_idle_add(page_switched_idle, NULL);
}

/* ---- find ------------------------------------------------------------ */

#define FIND_LIST_MAX 1000 /* occurrences listed per file */

static GtkWidget *g_findbar, *g_find_entry, *g_find_msg;
static gboolean g_find_case, g_find_all;
static gboolean g_find_sync; /* the entry is being set by code, not typed */

/* GtkTextIter's search has no case-insensitive mode in GTK2, so searches run
 * over arrays of code points (lowercased one-to-one when ignoring case), which
 * also keeps array indexes equal to buffer character offsets. */
static gunichar *fold_utf8(const char *s, long *n)
{
    glong cnt = 0;
    gunichar *u = g_utf8_to_ucs4_fast(s, -1, &cnt);
    if (!g_find_case)
        for (glong i = 0; i < cnt; i++)
            u[i] = g_unichar_tolower(u[i]);
    *n = cnt;
    return u;
}

static gunichar *doc_fold(Doc *d, long *n)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(d->buf, &a, &b);
    char *text = gtk_text_buffer_get_text(d->buf, &a, &b, TRUE);
    gunichar *u = fold_utf8(text, n);
    g_free(text);
    return u;
}

/* Forward: first match starting at or after FROM. Backward: last match
 * ending at or before FROM. */
static gboolean fold_search(const gunichar *t, long n, const gunichar *q, long qn, long from, gboolean fwd, long *at)
{
    if (qn == 0 || qn > n)
        return FALSE;
    long last = n - qn;
    long i = fwd ? (from < 0 ? 0 : from) : MIN(from - qn, last);
    for (; fwd ? i <= last : i >= 0; i += fwd ? 1 : -1)
        if (t[i] == q[0] && !memcmp(t + i, q, (size_t)qn * sizeof *q)) {
            *at = i;
            return TRUE;
        }
    return FALSE;
}

/* the document carrying the highlighted match */

/* An unfocused GtkTextView hides its selection, so the match is also tagged. */
static void find_unhighlight(void)
{
    if (!g_find_hl)
        return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(g_find_hl->buf, &a, &b);
    gtk_text_buffer_remove_tag_by_name(g_find_hl->buf, "find-match", &a, &b);
    g_find_hl = NULL;
}

static void find_msg(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_find_msg), text ? text : "");
}

static void find_goto(Doc *d, long a, long b, gboolean fwd, gboolean focus_entry)
{
    GtkNotebook *nb = GTK_NOTEBOOK(g_notebook);
    Doc *cur = doc_current();
    if (d != cur)
        gtk_notebook_set_current_page(nb, gtk_notebook_page_num(nb, d->scroll));
    GtkTextIter ia, ib;
    gtk_text_buffer_get_iter_at_offset(d->buf, &ia, (int)a);
    gtk_text_buffer_get_iter_at_offset(d->buf, &ib, (int)b);
    gtk_text_buffer_select_range(d->buf, fwd ? &ib : &ia, fwd ? &ia : &ib);
    find_unhighlight();
    gtk_text_buffer_apply_tag_by_name(d->buf, "find-match", &ia, &ib);
    g_find_hl = d;
    if (d == cur) {
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(d->view), gtk_text_buffer_get_insert(d->buf), 0.15, FALSE, 0, 0);
    } else {
        /* the page has no size until it is shown */
        g_object_ref(d->scroll);
        g_idle_add(scroll_to_cursor_idle, d);
    }
    if (focus_entry && !gtk_widget_has_focus(g_find_entry))
        gtk_widget_grab_focus(g_find_entry);
}

/* INCREMENTAL: the current match may be extended (typing), so search from the
 * start of the selection. */
static void find_step(gboolean fwd, gboolean incremental, gboolean focus_entry)
{
    Doc *cur = doc_current();
    const char *qs = gtk_entry_get_text(GTK_ENTRY(g_find_entry));
    find_msg(NULL);
    if (!cur || !*qs)
        return;
    long qn;
    gunichar *q = fold_utf8(qs, &qn);
    GtkNotebook *nb = GTK_NOTEBOOK(g_notebook);
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(cur->buf, &a, &b);
    long from = gtk_text_iter_get_offset(fwd && !incremental ? &b : &a);
    int n = gtk_notebook_get_n_pages(nb), idx = gtk_notebook_get_current_page(nb);
    int count = g_find_all ? n : 1;
    Doc *hit = NULL;
    long at = 0;
    /* step 0: the rest of this file; then the following files, and finally
     * this file again from its other end (the wrap-around) */
    for (int i = 0; i <= count && !hit; i++) {
        Doc *o = cur;
        if (g_find_all && i)
            o = doc_of(gtk_notebook_get_nth_page(nb, ((idx + (fwd ? i : -i)) % n + n) % n));
        long tn;
        gunichar *t = doc_fold(o, &tn);
        long start = i == 0 ? from : (fwd ? 0 : tn);
        if (fold_search(t, tn, q, qn, start, fwd, &at))
            hit = o;
        g_free(t);
    }
    if (hit)
        find_goto(hit, at, at + qn, fwd, focus_entry);
    else
        find_msg(_("Not found"));
    g_free(q);
}

static void find_next_cb(void)
{
    find_step(TRUE, FALSE, TRUE);
}

static void find_prev_cb(void)
{
    find_step(FALSE, FALSE, TRUE);
}

static void find_close_cb(void)
{
    gtk_widget_hide(g_findbar);
    find_unhighlight();
    Doc *d = doc_current();
    if (d)
        gtk_widget_grab_focus(d->view);
}

static void find_entry_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    (void)data;
    if (!g_find_sync)
        find_step(TRUE, TRUE, TRUE);
}

static gboolean find_entry_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->keyval == GDK_Return || ev->keyval == GDK_KP_Enter || ev->keyval == GDK_ISO_Enter) {
        find_step(!(ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)), FALSE, TRUE);
        return TRUE;
    }
    if (ev->keyval == GDK_Escape) {
        find_close_cb();
        return TRUE;
    }
    return FALSE;
}

static void find_entry_set(const char *text)
{
    g_find_sync = TRUE;
    gtk_entry_set_text(GTK_ENTRY(g_find_entry), text);
    g_find_sync = FALSE;
}

static void find_show(void)
{
    Doc *d = doc_current();
    GtkTextIter a, b;
    /* a one-line selection becomes the search text */
    if (d && gtk_text_buffer_get_selection_bounds(d->buf, &a, &b) && gtk_text_iter_get_line(&a) == gtk_text_iter_get_line(&b) &&
        gtk_text_iter_get_offset(&b) - gtk_text_iter_get_offset(&a) <= 200) {
        char *sel = gtk_text_buffer_get_text(d->buf, &a, &b, TRUE);
        find_entry_set(sel);
        g_free(sel);
    }
    find_msg(NULL);
    gtk_widget_show(g_findbar);
    gtk_widget_grab_focus(g_find_entry);
    gtk_editable_select_region(GTK_EDITABLE(g_find_entry), 0, -1);
}

/* -- the "+" dialog: options and the per-file list of occurrences -- */

enum { FC_TEXT, FC_PAGE, FC_OFF, FC_COLS };

/* The text of the line holding OFF, trimmed and shortened for the list. */
static char *line_snippet(Doc *d, long off, int *line)
{
    GtkTextIter it, s, e;
    gtk_text_buffer_get_iter_at_offset(d->buf, &it, (int)off);
    *line = gtk_text_iter_get_line(&it) + 1;
    s = e = it;
    gtk_text_iter_set_line_offset(&s, 0);
    if (!gtk_text_iter_ends_line(&e))
        gtk_text_iter_forward_to_line_end(&e);
    char *text = g_strstrip(gtk_text_buffer_get_text(d->buf, &s, &e, TRUE));
    glong len = g_utf8_strlen(text, -1);
    *g_utf8_offset_to_pointer(text, MIN(len, 120)) = '\0';
    return text;
}

static void find_list_fill(GtkTreeStore *st)
{
    gtk_tree_store_clear(st);
    const char *qs = gtk_entry_get_text(GTK_ENTRY(g_find_entry));
    if (!*qs)
        return;
    long qn;
    gunichar *q = fold_utf8(qs, &qn);
    GtkNotebook *nb = GTK_NOTEBOOK(g_notebook);
    int n = gtk_notebook_get_n_pages(nb), cur = gtk_notebook_get_current_page(nb);
    for (int p = 0; p < n; p++) {
        if (!g_find_all && p != cur)
            continue;
        Doc *d = doc_of(gtk_notebook_get_nth_page(nb, p));
        long tn, at, from = 0;
        gunichar *t = doc_fold(d, &tn);
        GArray *hits = g_array_new(FALSE, FALSE, sizeof(long));
        while (hits->len < FIND_LIST_MAX && fold_search(t, tn, q, qn, from, TRUE, &at)) {
            g_array_append_val(hits, at);
            from = at + qn;
        }
        g_free(t);
        if (hits->len) {
            char *name = doc_name(d);
            char *title = g_strdup_printf("%s (%u)", name, hits->len);
            GtkTreeIter par, ch;
            gtk_tree_store_append(st, &par, NULL);
            gtk_tree_store_set(st, &par, FC_TEXT, title, FC_PAGE, d->scroll, FC_OFF, -1, -1);
            g_free(title);
            g_free(name);
            for (guint i = 0; i < hits->len; i++) {
                long off = g_array_index(hits, long, i);
                int line;
                char *snip = line_snippet(d, off, &line);
                char *text = g_strdup_printf("%d: %s", line, snip);
                gtk_tree_store_append(st, &ch, &par);
                gtk_tree_store_set(st, &ch, FC_TEXT, text, FC_PAGE, d->scroll, FC_OFF, (int)off, -1);
                g_free(text);
                g_free(snip);
            }
        }
        g_array_free(hits, TRUE);
    }
    g_free(q);
    GtkWidget *tv = g_object_get_data(G_OBJECT(st), "tv");
    if (tv)
        gtk_tree_view_expand_all(GTK_TREE_VIEW(tv));
}

/* A file row goes to its first occurrence, an occurrence row to itself. */
static void find_list_goto(GtkTreeView *tv, GtkTreePath *path)
{
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it, ch;
    gpointer page;
    int off;
    if (!gtk_tree_model_get_iter(m, &it, path))
        return;
    gtk_tree_model_get(m, &it, FC_PAGE, &page, FC_OFF, &off, -1);
    if (off < 0) {
        if (!gtk_tree_model_iter_children(m, &ch, &it))
            return;
        gtk_tree_model_get(m, &ch, FC_OFF, &off, -1);
    }
    long qn = g_utf8_strlen(gtk_entry_get_text(GTK_ENTRY(g_find_entry)), -1);
    find_goto(doc_of(GTK_WIDGET(page)), off, off + qn, TRUE, FALSE);
}

static gboolean find_list_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)data;
    GtkTreePath *path = NULL;
    if (ev->button == 1 && gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL)) {
        find_list_goto(GTK_TREE_VIEW(w), path);
        gtk_tree_path_free(path);
    }
    return FALSE;
}

static void find_list_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer dlg)
{
    (void)col;
    find_list_goto(tv, path);
    gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);
}

static void find_dlg_entry_changed(GtkEditable *e, gpointer store)
{
    find_entry_set(gtk_entry_get_text(GTK_ENTRY(e)));
    find_msg(NULL);
    find_list_fill(store);
}

static void find_dlg_case_toggled(GtkToggleButton *b, gpointer store)
{
    g_find_case = gtk_toggle_button_get_active(b);
    find_list_fill(store);
}

static void find_dlg_all_toggled(GtkToggleButton *b, gpointer store)
{
    g_find_all = gtk_toggle_button_get_active(b);
    find_list_fill(store);
}

static void find_dlg_step_cb(GtkWidget *b, gpointer fwd)
{
    (void)b;
    find_step(GPOINTER_TO_INT(fwd), FALSE, FALSE);
}

static void find_more_cb(void)
{
    gtk_widget_show(g_findbar);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Find"), GTK_WINDOW(g_win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, 420);
    GtkWidget *box = GTK_DIALOG(dlg)->vbox;
    gtk_box_set_spacing(GTK_BOX(box), 6);

    GtkTreeStore *store = gtk_tree_store_new(FC_COLS, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT);

    GtkWidget *row = gtk_hbox_new(FALSE, 4);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), gtk_entry_get_text(GTK_ENTRY(g_find_entry)));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), FALSE);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);
    const char *stock[2] = { GTK_STOCK_GO_DOWN, GTK_STOCK_GO_UP };
    const char *tips[2] = { _("Next occurrence"), _("Previous occurrence") };
    for (int i = 0; i < 2; i++) {
        GtkWidget *btn = gtk_button_new();
        gtk_container_add(GTK_CONTAINER(btn), gtk_image_new_from_stock(stock[i], GTK_ICON_SIZE_BUTTON));
        gtk_widget_set_tooltip_text(btn, tips[i]);
        g_signal_connect(btn, "clicked", G_CALLBACK(find_dlg_step_cb), GINT_TO_POINTER(i == 0));
        gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    GtkWidget *cs = gtk_check_button_new_with_mnemonic(_("_Match case"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs), g_find_case);
    GtkWidget *all = gtk_check_button_new_with_mnemonic(_("Search in _all open files"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(all), g_find_all);
    gtk_box_pack_start(GTK_BOX(box), cs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), all, FALSE, FALSE, 0);

    GtkWidget *lbl = gtk_label_new(_("Occurrences"));
    gtk_misc_set_alignment(GTK_MISC(lbl), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
                                gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", FC_TEXT, NULL));
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    g_signal_connect(entry, "changed", G_CALLBACK(find_dlg_entry_changed), store);
    g_signal_connect(cs, "toggled", G_CALLBACK(find_dlg_case_toggled), store);
    g_signal_connect(all, "toggled", G_CALLBACK(find_dlg_all_toggled), store);
    g_signal_connect(tv, "button-release-event", G_CALLBACK(find_list_release), NULL);
    g_signal_connect(tv, "row-activated", G_CALLBACK(find_list_activated), dlg);

    g_object_set_data(G_OBJECT(store), "tv", tv);
    find_list_fill(store);
    gtk_widget_show_all(box);
    gtk_widget_grab_focus(entry);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_object_unref(store);
    gtk_widget_grab_focus(g_find_entry);
}

static GtkWidget *find_button(const char *stock, const char *tip, GCallback cb)
{
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
    gtk_container_add(GTK_CONTAINER(btn), gtk_image_new_from_stock(stock, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(btn, tip);
    g_signal_connect(btn, "clicked", cb, NULL);
    return btn;
}

static GtkWidget *build_findbar(void)
{
    g_findbar = gtk_hbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(g_findbar), 2);
    g_find_entry = gtk_entry_new();
    g_signal_connect(g_find_entry, "changed", G_CALLBACK(find_entry_changed), NULL);
    g_signal_connect(g_find_entry, "key-press-event", G_CALLBACK(find_entry_key), NULL);
    g_find_msg = gtk_label_new("");
    gtk_misc_set_padding(GTK_MISC(g_find_msg), 6, 0);
    gtk_box_pack_start(GTK_BOX(g_findbar), g_find_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_findbar), find_button(GTK_STOCK_GO_DOWN, _("Next occurrence (Enter)"), G_CALLBACK(find_next_cb)),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_findbar),
                       find_button(GTK_STOCK_GO_UP, _("Previous occurrence (Ctrl+Enter)"), G_CALLBACK(find_prev_cb)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_findbar), g_find_msg, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(g_findbar), find_button(GTK_STOCK_CLOSE, _("Close search (Esc)"), G_CALLBACK(find_close_cb)), FALSE,
                     FALSE, 0);
    gtk_box_pack_end(GTK_BOX(g_findbar), find_button(GTK_STOCK_ADD, _("More options..."), G_CALLBACK(find_more_cb)), FALSE, FALSE,
                     0);
    gtk_widget_show_all(g_findbar);
    gtk_widget_set_no_show_all(g_findbar, TRUE);
    gtk_widget_hide(g_findbar);
    return g_findbar;
}

/* ---- window ---------------------------------------------------------- */

static GtkWidget *menu_item(GtkWidget *menu, GtkAccelGroup *ag, const char *label, guint key, GdkModifierType mods,
                            GCallback cb)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    if (key)
        gtk_widget_add_accelerator(it, "activate", ag, key, mods, GTK_ACCEL_VISIBLE);
    g_signal_connect(it, "activate", cb, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label)
{
    GtkWidget *m = gtk_menu_new();
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), m);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), it);
    return m;
}

static void separator(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static GtkWidget *build_menubar(GtkAccelGroup *ag)
{
    GtkWidget *bar = gtk_menu_bar_new();
    const GdkModifierType C = GDK_CONTROL_MASK, CS = GDK_CONTROL_MASK | GDK_SHIFT_MASK;

    GtkWidget *m = submenu(bar, _("_File"));
    menu_item(m, ag, _("_New"), GDK_n, C, G_CALLBACK(new_cb));
    menu_item(m, ag, _("_Open..."), GDK_o, C, G_CALLBACK(open_cb));
    separator(m);
    menu_item(m, ag, _("_Save"), GDK_s, C, G_CALLBACK(save_cb));
    menu_item(m, ag, _("Save _as..."), GDK_s, CS, G_CALLBACK(save_as_cb));
    GtkWidget *rit = gtk_menu_item_new_with_mnemonic(_("Recent _files"));
    GtkWidget *rm = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(rit), rm);
    g_signal_connect(rm, "show", G_CALLBACK(recent_menu_fill), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), rit);
    separator(m);
    menu_item(m, ag, _("_Reopen file"), 0, 0, G_CALLBACK(reopen_cb));
    menu_item(m, ag, _("_Close tab"), GDK_w, C, G_CALLBACK(close_cb));
    menu_item(m, ag, _("_Quit"), GDK_q, C, G_CALLBACK(quit_cb));

    m = submenu(bar, _("_Edit"));
    menu_item(m, ag, _("_Undo"), GDK_z, C, G_CALLBACK(undo_cb));
    menu_item(m, ag, _("_Redo"), GDK_z, CS, G_CALLBACK(redo_cb));
    separator(m);
    menu_item(m, ag, _("Cu_t"), GDK_x, C, G_CALLBACK(cut_cb));
    menu_item(m, ag, _("_Copy"), GDK_c, C, G_CALLBACK(copy_cb));
    menu_item(m, ag, _("_Paste"), GDK_v, C, G_CALLBACK(paste_cb));
    separator(m);
    menu_item(m, ag, _("Select _all"), GDK_a, C, G_CALLBACK(select_all_cb));
    separator(m);
    menu_item(m, ag, _("_Find..."), GDK_f, C, G_CALLBACK(find_show));

    m = submenu(bar, _("_View"));
    g_linenum_item = gtk_check_menu_item_new_with_mnemonic(_("_Line numbers"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_linenum_item), g_line_numbers);
    g_signal_connect(g_linenum_item, "toggled", G_CALLBACK(linenum_toggle_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), g_linenum_item);
    g_wrap_item = gtk_check_menu_item_new_with_mnemonic(_("_Word wrap"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_wrap_item), g_word_wrap);
    g_signal_connect(g_wrap_item, "toggled", G_CALLBACK(wrap_toggle_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), g_wrap_item);
    GtkWidget *syntax_item = gtk_check_menu_item_new_with_mnemonic(_("S_yntax highlighting"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(syntax_item), g_syntax);
    g_signal_connect(syntax_item, "toggled", G_CALLBACK(syntax_toggle_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), syntax_item);
    GtkWidget *single_item = gtk_check_menu_item_new_with_mnemonic(_("Open files in the _same window"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(single_item), g_single);
    g_signal_connect(single_item, "toggled", G_CALLBACK(single_toggle_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), single_item);
    separator(m);
    menu_item(m, ag, _("_Font..."), 0, 0, G_CALLBACK(font_cb));
    menu_item(m, ag, _("_Tab key..."), 0, 0, G_CALLBACK(tabs_cb));
    menu_item(m, ag, _("_Autosave..."), 0, 0, G_CALLBACK(autosave_cb));
    menu_item(m, ag, _("Zoom _in"), GDK_plus, C, G_CALLBACK(zoom_in_cb));
    menu_item(m, ag, _("Zoom _out"), GDK_minus, C, G_CALLBACK(zoom_out_cb));
    menu_item(m, ag, _("_Reset zoom"), GDK_0, C, G_CALLBACK(zoom_reset_cb));

    m = submenu(bar, _("_Help"));
    menu_item(m, ag, _("_About"), 0, 0, G_CALLBACK(about_cb));
    return bar;
}

/* ---- line editing and window shortcuts --------------------------------- */

/* The lines the selection touches (a selection ending at the start of a line
 * does not include that line). */
static void sel_lines(Doc *d, int *l1, int *l2)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_selection_bounds(d->buf, &a, &b); /* ordered; both = cursor if empty */
    *l1 = gtk_text_iter_get_line(&a);
    *l2 = gtk_text_iter_get_line(&b);
    if (*l2 > *l1 && gtk_text_iter_starts_line(&b))
        (*l2)--;
}

static char *lines_text(Doc *d, int l1, int l2)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(d->buf, &a, l1);
    gtk_text_buffer_get_iter_at_line(d->buf, &b, l2);
    if (!gtk_text_iter_ends_line(&b))
        gtk_text_iter_forward_to_line_end(&b);
    return gtk_text_buffer_get_text(d->buf, &a, &b, TRUE);
}

static void sel_restore(Doc *d, int ins, int bound)
{
    GtkTextIter i, b;
    gtk_text_buffer_get_iter_at_offset(d->buf, &i, ins);
    gtk_text_buffer_get_iter_at_offset(d->buf, &b, bound);
    gtk_text_buffer_select_range(d->buf, &i, &b);
}

/* Alt+Up / Alt+Down: swaps the selected lines with the neighbour above/below. */
static void move_lines(gboolean up)
{
    Doc *d = doc_current();
    if (!d)
        return;
    int l1, l2, last = gtk_text_buffer_get_line_count(d->buf) - 1;
    sel_lines(d, &l1, &l2);
    if (up ? l1 == 0 : l2 == last)
        return;
    int ni = up ? l1 - 1 : l2 + 1;
    char *block = lines_text(d, l1, l2), *neigh = lines_text(d, ni, ni);
    int shift = (int)g_utf8_strlen(neigh, -1) + 1;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(d->buf, &it, gtk_text_buffer_get_insert(d->buf));
    int ins = gtk_text_iter_get_offset(&it);
    gtk_text_buffer_get_iter_at_mark(d->buf, &it, gtk_text_buffer_get_selection_bound(d->buf));
    int bound = gtk_text_iter_get_offset(&it);
    char *swapped = up ? g_strconcat(block, "\n", neigh, NULL) : g_strconcat(neigh, "\n", block, NULL);
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(d->buf, &a, up ? ni : l1);
    gtk_text_buffer_get_iter_at_line(d->buf, &b, up ? l2 : ni);
    if (!gtk_text_iter_ends_line(&b))
        gtk_text_iter_forward_to_line_end(&b);
    gtk_text_buffer_begin_user_action(d->buf);
    gtk_text_buffer_delete(d->buf, &a, &b);
    gtk_text_buffer_insert(d->buf, &a, swapped, -1);
    gtk_text_buffer_end_user_action(d->buf);
    int delta = up ? -shift : shift;
    sel_restore(d, ins + delta, bound + delta);
    GtkTextMark *m = gtk_text_buffer_get_insert(d->buf);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(d->view), m);
    g_free(block);
    g_free(neigh);
    g_free(swapped);
}

/* Alt+Shift+Up / Down: copies the selected lines above/below (the selection
 * follows the copy that goes down, like VS Code). */
static void copy_lines(gboolean up)
{
    Doc *d = doc_current();
    if (!d)
        return;
    int l1, l2;
    sel_lines(d, &l1, &l2);
    char *block = lines_text(d, l1, l2);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(d->buf, &it, gtk_text_buffer_get_insert(d->buf));
    int ins = gtk_text_iter_get_offset(&it);
    gtk_text_buffer_get_iter_at_mark(d->buf, &it, gtk_text_buffer_get_selection_bound(d->buf));
    int bound = gtk_text_iter_get_offset(&it);
    char *text = g_strconcat("\n", block, NULL);
    gtk_text_buffer_get_iter_at_line(d->buf, &it, l2);
    if (!gtk_text_iter_ends_line(&it))
        gtk_text_iter_forward_to_line_end(&it);
    gtk_text_buffer_begin_user_action(d->buf);
    gtk_text_buffer_insert(d->buf, &it, text, -1);
    gtk_text_buffer_end_user_action(d->buf);
    int delta = up ? 0 : (int)g_utf8_strlen(text, -1);
    sel_restore(d, ins + delta, bound + delta);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(d->view), gtk_text_buffer_get_insert(d->buf));
    g_free(block);
    g_free(text);
}

static void cycle_tab(int dir)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    if (n < 2)
        return;
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), (cur + dir + n) % n);
}

static void toggle_fullscreen(void)
{
    static gboolean full;
    full = !full;
    if (full)
        gtk_window_fullscreen(GTK_WINDOW(g_win));
    else
        gtk_window_unfullscreen(GTK_WINDOW(g_win));
}

/* Ctrl+= is Ctrl+plus on keyboards where plus needs Shift. */
static void on_notebook_style(GtkWidget *w, GtkStyle *prev, gpointer d)
{
    (void)prev;
    (void)d;
    GtkStyle *theme = gtk_rc_get_style_by_paths(gtk_widget_get_settings(w), NULL, NULL, GTK_TYPE_NOTEBOOK);
    guint hb = 2 + (theme ? theme->xthickness : 0);
    guint cur = 0;
    g_object_get(w, "tab-hborder", &cur, NULL);
    if (cur != hb)
        g_object_set(w, "tab-hborder", hb, NULL);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (ev->keyval == GDK_F11 && !mods) {
        toggle_fullscreen();
        return TRUE;
    }
    if ((mods & GDK_CONTROL_MASK) && (ev->keyval == GDK_Tab || ev->keyval == GDK_ISO_Left_Tab)) {
        cycle_tab((mods & GDK_SHIFT_MASK) || ev->keyval == GDK_ISO_Left_Tab ? -1 : 1);
        return TRUE;
    }
    if ((ev->keyval == GDK_Up || ev->keyval == GDK_Down) && (mods == GDK_MOD1_MASK || mods == (GDK_MOD1_MASK | GDK_SHIFT_MASK))) {
        if (mods & GDK_SHIFT_MASK)
            copy_lines(ev->keyval == GDK_Up);
        else
            move_lines(ev->keyval == GDK_Up);
        return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && (ev->keyval == GDK_equal || ev->keyval == GDK_KP_Add)) {
        zoom(1);
        return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_KP_Subtract) {
        zoom(-1);
        return TRUE;
    }
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_y) {
        redo_cb();
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    xis_i18n_init("kipad");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("kipad %s\n", KIPAD_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("kipad %s - simple tabbed text editor\n\nUsage: kipad [-n|--new-window] [FILE...]\n\n"
                   "With \"Open files in the same window\" on (the default), FILEs open as tabs of a\n"
                   "running kipad; --new-window always starts a window of its own.\n", KIPAD_VERSION);
            return 0;
        }
    }
    gtk_init(&argc, &argv);
    config_load();
    autosave_restart();
    syn_set_enabled(g_syntax);
    syn_set_changed_cb(status_update);
    gboolean new_window = FALSE;
    char **files = g_new0(char *, argc + 1);
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            while (++i < argc)
                files[nfiles++] = argv[i];
        } else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--new-window"))
            new_window = TRUE;
        else if (argv[i][0] != '-')
            files[nfiles++] = argv[i];
    }
    if (g_single && !new_window && handover_to_running(files, nfiles)) {
        g_free(files);
        return 0;
    }
    gtk_window_set_default_icon_name("kipad");

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(g_win), 820, 600);
    xis_winstate_restore(GTK_WINDOW(g_win), "kipad", XIS_WINSTATE_ALL);
    xis_winstate_track(GTK_WINDOW(g_win), "kipad", XIS_WINSTATE_ALL);
    g_signal_connect(g_win, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(g_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(g_win), ag);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_win), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menubar(ag), FALSE, FALSE, 0);

    /* No side frame on the notebook and the scroll bar outside the view's
     * frame: the bar reaches the window's edge, so a maximized window's can
     * be grabbed by slamming the pointer into the screen edge. The tabs get
     * the lost padding back as tab-hborder. */
    gtk_rc_parse_string("style \"kipad-tabs\" { xthickness = 0 }\n"
                        "widget \"*.kipad-tabs\" style \"kipad-tabs\"\n"
                        "style \"kipad-view-sw\" { GtkScrolledWindow::scrollbars-within-bevel = 0 }\n"
                        "widget \"*.kipad-view-sw\" style \"kipad-view-sw\"\n");
    g_notebook = gtk_notebook_new();
    gtk_widget_set_name(g_notebook, "kipad-tabs");
    gtk_notebook_set_show_border(GTK_NOTEBOOK(g_notebook), FALSE);
    g_signal_connect(g_notebook, "style-set", G_CALLBACK(on_notebook_style), NULL);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_notebook), TRUE);
    g_signal_connect(g_notebook, "switch-page", G_CALLBACK(page_switched), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_notebook, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_findbar(), FALSE, FALSE, 0);

    g_status = gtk_hbox_new(FALSE, 0);
    g_pos_label = gtk_label_new("");
    gtk_misc_set_padding(GTK_MISC(g_pos_label), 8, 2);
    gtk_box_pack_end(GTK_BOX(g_status), g_pos_label, FALSE, FALSE, 0);
    g_lang_label = gtk_label_new("");
    gtk_misc_set_padding(GTK_MISC(g_lang_label), 8, 2);
    gtk_box_pack_end(GTK_BOX(g_status), g_lang_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    gtk_widget_show_all(g_win);

    if (g_single && !new_window)
        service_start();
    for (int i = 0; i < nfiles; i++)
        open_path(files[i]);
    if (!nfiles) {
        /* no files given: reopen the tabs the last window had */
        State s;
        state_load(&s);
        int active = -1, opened = 0;
        for (guint i = 0; i < s.tabs->len; i++) {
            const char *p = g_ptr_array_index(s.tabs, i);
            if (!g_file_test(p, G_FILE_TEST_IS_REGULAR))
                continue;
            open_path(p);
            if ((int)i == s.active)
                active = opened;
            opened++;
        }
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
        if (n > 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), active >= 0 && active < n ? active : n - 1);
        state_clear(&s);
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 0)
        new_empty_tab();
    status_update();
    gtk_main();
    service_stop();
    g_free(files);
    return 0;
}
