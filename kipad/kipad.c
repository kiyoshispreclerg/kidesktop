/* kipad - tabbed text editor for KiDesktop, GTK2.
 *
 * One process per window. Each tab is a GtkTextView in a scrolled window;
 * its Doc (file name, undo history) hangs off the scrolled window. Settings
 * (font, line numbers) live in ~/.config/kipad.conf and are written whenever
 * they change. */
#include "../shared/xis_i18n.h"
#include "../shared/xis_safefile.h"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KIPAD_VERSION "0.1.1"
#define DEFAULT_FONT "Monospace 11"
#define MIN_FONT_PT 6
#define MAX_FONT_PT 72
#define UNDO_LIMIT 1000

/* ---- settings -------------------------------------------------------- */

static char g_font[128] = DEFAULT_FONT;
static gboolean g_line_numbers = TRUE;
static gboolean g_word_wrap = FALSE;

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
    char *text = g_strdup_printf("# kipad settings, written by kipad\nfont = %s\nline_numbers = %d\nword_wrap = %d\n",
                                 g_font, g_line_numbers, g_word_wrap);
    xis_write_file(path, text, strlen(text), xis_check_conf_clean);
    g_free(text);
    g_free(dir);
    g_free(path);
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
} Doc;

static GtkWidget *g_win, *g_notebook, *g_status, *g_pos_label;
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

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w;
    (void)data;
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
    linenum_resize(d);
    gtk_widget_queue_draw(d->view);
    if (d == doc_current())
        status_update(); /* typing moves the cursor without a mark-set */
}

static void on_modified(GtkTextBuffer *buf, Doc *d)
{
    (void)buf;
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

static void doc_free(Doc *d)
{
    g_array_free(d->undo, TRUE);
    g_array_free(d->redo, TRUE);
    g_free(d->path);
    g_free(d);
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
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(d->scroll), GTK_SHADOW_IN);
    d->view = gtk_text_view_new();
    d->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->view));
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d->view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(d->view), 4);
    gtk_container_add(GTK_CONTAINER(d->scroll), d->view);
    g_object_set_data_full(G_OBJECT(d->scroll), "doc", d, (GDestroyNotify)doc_free);
    apply_font_to(d);
    apply_wrap_to(d);
    linenum_resize(d);

    g_signal_connect(d->buf, "insert-text", G_CALLBACK(on_insert_text), d);
    g_signal_connect(d->buf, "delete-range", G_CALLBACK(on_delete_range), d);
    g_signal_connect(d->buf, "changed", G_CALLBACK(on_changed), d);
    g_signal_connect(d->buf, "modified-changed", G_CALLBACK(on_modified), d);
    g_signal_connect(d->buf, "mark-set", G_CALLBACK(on_mark_set), d);
    g_signal_connect(d->view, "expose-event", G_CALLBACK(on_expose), d);
    g_signal_connect(d->view, "scroll-event", G_CALLBACK(on_scroll), NULL);

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
    gtk_widget_show_all(box);

    gtk_widget_show_all(d->scroll);
    int n = gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), d->scroll, box);
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

static void message(GtkMessageType type, const char *fmt, const char *arg)
{
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, fmt, arg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
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
    char *data = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(abs, &data, &len, &err)) {
        message(GTK_MESSAGE_ERROR, _("Could not open the file:\n%s"), err->message);
        g_error_free(err);
        g_free(abs);
        return;
    }
    char *utf8 = NULL;
    if (g_utf8_validate(data, (gssize)len, NULL)) {
        utf8 = data;
    } else {
        /* not UTF-8: assume the usual legacy encoding */
        utf8 = g_convert(data, (gssize)len, "UTF-8", "ISO-8859-1", NULL, &len, NULL);
        g_free(data);
    }
    if (!utf8) {
        message(GTK_MESSAGE_ERROR, _("Could not read the file:\n%s"), abs);
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
    doc_set_text(d, utf8, len);
    g_free(utf8);
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
    g_free(text);
    if (!ok) {
        message(GTK_MESSAGE_ERROR, _("Could not save the file:\n%s"), g_strerror(errno));
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
        if (ok)
            g_free(old);
        else {
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
    gtk_notebook_remove_page(GTK_NOTEBOOK(g_notebook), n);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 0)
        gtk_main_quit();
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), i);
        if (!confirm_discard(doc_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i))))
            return TRUE;
    }
    return FALSE;
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
    separator(m);
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

    m = submenu(bar, _("_View"));
    g_linenum_item = gtk_check_menu_item_new_with_mnemonic(_("_Line numbers"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_linenum_item), g_line_numbers);
    g_signal_connect(g_linenum_item, "toggled", G_CALLBACK(linenum_toggle_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), g_linenum_item);
    g_wrap_item = gtk_check_menu_item_new_with_mnemonic(_("_Word wrap"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_wrap_item), g_word_wrap);
    g_signal_connect(g_wrap_item, "toggled", G_CALLBACK(wrap_toggle_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), g_wrap_item);
    separator(m);
    menu_item(m, ag, _("_Font..."), 0, 0, G_CALLBACK(font_cb));
    menu_item(m, ag, _("Zoom _in"), GDK_plus, C, G_CALLBACK(zoom_in_cb));
    menu_item(m, ag, _("Zoom _out"), GDK_minus, C, G_CALLBACK(zoom_out_cb));
    menu_item(m, ag, _("_Reset zoom"), GDK_0, C, G_CALLBACK(zoom_reset_cb));

    m = submenu(bar, _("_Help"));
    menu_item(m, ag, _("_About"), 0, 0, G_CALLBACK(about_cb));
    return bar;
}

/* Ctrl+= is Ctrl+plus on keyboards where plus needs Shift. */
static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
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
            printf("kipad %s - simple tabbed text editor\n\nUsage: kipad [FILE...]\n", KIPAD_VERSION);
            return 0;
        }
    }
    gtk_init(&argc, &argv);
    config_load();
    gtk_window_set_default_icon_name("kipad");

    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(g_win), 820, 600);
    g_signal_connect(g_win, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(g_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(g_win), ag);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(g_win), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menubar(ag), FALSE, FALSE, 0);

    g_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_notebook), TRUE);
    g_signal_connect(g_notebook, "switch-page", G_CALLBACK(page_switched), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_notebook, TRUE, TRUE, 0);

    g_status = gtk_hbox_new(FALSE, 0);
    g_pos_label = gtk_label_new("");
    gtk_misc_set_padding(GTK_MISC(g_pos_label), 8, 2);
    gtk_box_pack_end(GTK_BOX(g_status), g_pos_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    gtk_widget_show_all(g_win);

    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            if (argv[i][0] != '-')
                open_path(argv[i]);
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 0)
        new_empty_tab();
    status_update();
    gtk_main();
    return 0;
}
