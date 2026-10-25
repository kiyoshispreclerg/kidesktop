/* kipaint - paint program for KiDesktop, GTK2, in the spirit of the
 * classic Paint.
 *
 * This file: window, menus, keyboard shortcuts, new/open/save and the
 * status bar. The canvas (kp_canvas.c), tools (tools/), palette and
 * toolbox each live in their own file; kp.h has the shared state. See
 * PLAN.md for where this is going. */
#include "kp.h"

#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <string.h>

KpState kp;

static GtkWidget *g_pos_label, *g_size_label, *g_zoom_label;
static GtkWidget *g_undo_item, *g_redo_item, *g_grid_item, *g_export_item, *g_panel_item;
static guint g_saved_state;
/* "Don't ask again" in the flatten notice, for this session only. */
static gboolean g_flatten_nag_off;
/* Where Export last went, and in which format. */
static char *g_export_dir;
static const char *g_export_ext = "png";

/* ---- title and status ------------------------------------------------ */

static gboolean doc_modified(void)
{
    return kp_undo_state() != g_saved_state;
}

static char *doc_name(void)
{
    return kp.doc->path ? g_path_get_basename(kp.doc->path) : g_strdup(_("Untitled"));
}

static void update_title(void)
{
    char *name = doc_name();
    char *title = g_strdup_printf("%s%s - kipaint", doc_modified() ? "*" : "", name);
    gtk_window_set_title(GTK_WINDOW(kp.win), title);
    g_free(title);
    g_free(name);
    /* flat formats are Export only in a layered project */
    if (kp.doc->layered)
        gtk_widget_show(g_export_item);
    else
        gtk_widget_hide(g_export_item);
}

void kp_status_pointer(int x, int y, gboolean inside)
{
    if (!g_pos_label)
        return;
    char buf[64];
    if (inside)
        g_snprintf(buf, sizeof(buf), _("%d, %d px"), x, y);
    else
        buf[0] = '\0';
    gtk_label_set_text(GTK_LABEL(g_pos_label), buf);
    g_snprintf(buf, sizeof(buf), "%.0f%%", kp_canvas_zoom() * 100);
    gtk_label_set_text(GTK_LABEL(g_zoom_label), buf);
}

static void update_size_label(void)
{
    char buf[64];
    g_snprintf(buf, sizeof(buf), _("%d x %d px"), kp.doc->w, kp.doc->h);
    gtk_label_set_text(GTK_LABEL(g_size_label), buf);
}

void kp_doc_edited(void)
{
    update_title();
    gtk_widget_set_sensitive(g_undo_item, kp_undo_can_undo());
    gtk_widget_set_sensitive(g_redo_item, kp_undo_can_redo());
}

static void message(GtkMessageType type, const char *fmt, const char *arg)
{
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(kp.win), GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, fmt, arg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ---- documents ------------------------------------------------------- */

static void set_doc(KpDoc *d)
{
    kp_layers_flush();
    kp_undo_clear();
    KpDoc *old = kp.doc;
    kp.doc = d;
    kp_doc_free(old);
    g_saved_state = kp_undo_state();
    kp_canvas_doc_changed();
    kp_layers_refresh();
    update_size_label();
    kp_doc_edited();
}

gboolean kp_open_path(const char *path)
{
    GError *err = NULL;
    KpDoc *d;
    if (kp_ora_is(path)) {
        d = kp_ora_load(path, &err);
    } else {
        cairo_surface_t *s = kp_io_load(path, &err);
        d = s ? kp_doc_new_from_surface(s) : NULL;
    }
    if (!d) {
        message(GTK_MESSAGE_ERROR, _("Could not open the image:\n%s"), err->message);
        g_error_free(err);
        return FALSE;
    }
    d->path = g_strdup(path);
    set_doc(d);
    return TRUE;
}

static gboolean save_as(void);

/* Writes the document to `path` in the format its extension names (.ora
 * keeps the layers; the flat formats flatten them). Shows the error. */
static gboolean write_path(const char *path)
{
    GError *err = NULL;
    gboolean ok = kp_ora_is(path) ? kp_ora_save(kp.doc, path, &err) : kp_io_save(kp.doc, path, &err);
    if (!ok) {
        message(GTK_MESSAGE_ERROR, _("Could not save the image:\n%s"), err->message);
        g_error_free(err);
    }
    return ok;
}

enum { FLATTEN_CONTINUE = 1, FLATTEN_SAVE_AS = 2 };

/* The notice before a flat format swallows the layers (PLAN.md 1.1). */
static int flatten_notice(void)
{
    char *fmt = kp_io_format_name(kp.doc->path);
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(kp.win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                            _("Saving as %s merges the layers into one image."), fmt);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s",
                                             _("The layers will not be kept in that file."));
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("Save _as..."), FLATTEN_SAVE_AS,
                           _("_Continue"), FLATTEN_CONTINUE, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), FLATTEN_SAVE_AS);
    GtkWidget *again = gtk_check_button_new_with_mnemonic(_("_Don't ask again in this session"));
    gtk_box_pack_end(GTK_BOX(GTK_DIALOG(dlg)->vbox), again, FALSE, FALSE, 4);
    gtk_widget_show(again);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    if (r == FLATTEN_CONTINUE && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(again)))
        g_flatten_nag_off = TRUE;
    gtk_widget_destroy(dlg);
    g_free(fmt);
    return r == FLATTEN_CONTINUE || r == FLATTEN_SAVE_AS ? r : 0;
}

static gboolean save(void)
{
    KpDoc *d = kp.doc;
    gboolean ora = d->path && kp_ora_is(d->path);
    if (!d->path || (!ora && !kp_io_writable(d->path)))
        return save_as();
    if (!ora && d->layers->len > 1 && !g_flatten_nag_off) {
        int r = flatten_notice();
        if (!r)
            return FALSE;
        if (r == FLATTEN_SAVE_AS)
            return save_as();
    }
    if (!write_path(d->path))
        return FALSE;
    d->layered = ora;
    g_saved_state = kp_undo_state();
    update_title();
    return TRUE;
}

static void chooser_set_start(GtkWidget *dlg, const char *ext)
{
    if (kp.doc->path && (kp_io_writable(kp.doc->path) || kp_ora_is(kp.doc->path)) &&
        (kp_ora_is(kp.doc->path) || kp.doc->layers->len < 2)) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), kp.doc->path);
        return;
    }
    /* a name in the format that keeps what the document has */
    char *name = doc_name();
    char *dot = strrchr(name, '.');
    if (dot && kp.doc->path)
        *dot = '\0';
    char *file = g_strdup_printf("%s.%s", name, ext);
    if (kp.doc->path) {
        char *dir = g_path_get_dirname(kp.doc->path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        g_free(dir);
    }
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), file);
    g_free(file);
    g_free(name);
}

static gboolean save_as(void)
{
    /* layers survive only in .ora */
    const char *def_ext = kp.doc->layered || kp.doc->layers->len > 1 ? "ora" : "png";
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Save as"), GTK_WINDOW(kp.win), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_SAVE,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    GtkWidget *hint = gtk_label_new(_("The extension picks the format: .ora keeps the layers; .png, .jpg, .bmp and "
                                      ".tiff merge them into one image."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(dlg), hint);
    chooser_set_start(dlg, def_ext);
    gboolean ok = FALSE;
    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (!kp_io_writable(path) && !kp_ora_is(path)) {
            char *with_ext = g_strdup_printf("%s.%s", path, def_ext);
            g_free(path);
            path = with_ext;
            /* the chooser only confirmed overwriting the name without the extension */
            if (g_file_test(path, G_FILE_TEST_EXISTS)) {
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), path);
                g_free(path);
                continue;
            }
        }
        ok = write_path(path);
        if (ok) {
            /* saving as .ora makes this a layered project; a flat format
             * makes it a flat image again, with that file as the target */
            g_free(kp.doc->path);
            kp.doc->path = path;
            kp.doc->layered = kp_ora_is(path);
            g_saved_state = kp_undo_state();
        } else {
            g_free(path);
        }
        break;
    }
    gtk_widget_destroy(dlg);
    update_title();
    return ok;
}

/* Layered projects only: writes a flat copy and leaves the document, its
 * path and its modified state alone. */
static void export_cb(void)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Export"), GTK_WINDOW(kp.win), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("_Export"),
                                                 GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    GtkWidget *hint = gtk_label_new(_("The extension picks the format: .png, .jpg, .bmp or .tiff. The layers are merged "
                                      "into the exported file only."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(dlg), hint);
    char *name = doc_name();
    char *dot = strrchr(name, '.');
    if (dot)
        *dot = '\0';
    char *file = g_strdup_printf("%s.%s", name, g_export_ext);
    char *dir = g_export_dir ? g_strdup(g_export_dir) : kp.doc->path ? g_path_get_dirname(kp.doc->path) : NULL;
    if (dir)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), file);
    g_free(dir);
    g_free(file);
    g_free(name);
    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (!kp_io_writable(path)) {
            char *with_ext = g_strdup_printf("%s.%s", path, g_export_ext);
            g_free(path);
            path = with_ext;
            if (g_file_test(path, G_FILE_TEST_EXISTS)) {
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), path);
                g_free(path);
                continue;
            }
        }
        GError *err = NULL;
        if (kp_io_save(kp.doc, path, &err)) {
            g_free(g_export_dir);
            g_export_dir = g_path_get_dirname(path);
            char *fmt = kp_io_format_name(path);
            g_export_ext = !g_ascii_strcasecmp(fmt, "JPG") || !g_ascii_strcasecmp(fmt, "JPEG") ? "jpg"
                           : !g_ascii_strcasecmp(fmt, "BMP")                                   ? "bmp"
                           : !g_ascii_strncasecmp(fmt, "TIF", 3)                               ? "tiff"
                                                                                               : "png";
            g_free(fmt);
        } else {
            message(GTK_MESSAGE_ERROR, _("Could not save the image:\n%s"), err->message);
            g_error_free(err);
        }
        g_free(path);
        break;
    }
    gtk_widget_destroy(dlg);
}

/* TRUE when it is fine to discard the image (saved, or the user said so). */
static gboolean confirm_discard(void)
{
    if (!doc_modified())
        return TRUE;
    char *name = doc_name();
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(kp.win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                            _("Save changes to \"%s\" before closing?"), name);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), _("Close without saving"), GTK_RESPONSE_REJECT, GTK_STOCK_CANCEL,
                           GTK_RESPONSE_CANCEL, GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(name);
    if (r == GTK_RESPONSE_REJECT)
        return TRUE;
    return r == GTK_RESPONSE_ACCEPT && save();
}

/* ---- menu actions ---------------------------------------------------- */

static GtkWidget *spin_row(GtkWidget *table, int row, const char *label, int value)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    GtkWidget *s = gtk_spin_button_new_with_range(1, 32767, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    gtk_entry_set_activates_default(GTK_ENTRY(s), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), s);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, row, row + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), s, 1, 2, row, row + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    return s;
}

static void new_cb(void)
{
    if (!confirm_discard())
        return;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("New image"), GTK_WINDOW(kp.win), GTK_DIALOG_MODAL,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_NEW,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 12);
    gtk_container_set_border_width(GTK_CONTAINER(table), 12);
    GtkWidget *ws = spin_row(table, 0, _("_Width (px):"), kp.new_w);
    GtkWidget *hs = spin_row(table, 1, _("_Height (px):"), kp.new_h);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), table, TRUE, TRUE, 0);
    gtk_widget_show_all(table);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        int w = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ws));
        int h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(hs));
        KpColor white = { 1, 1, 1, 1 };
        KpDoc *d = kp_doc_new(w, h, &white);
        if (d) {
            kp.new_w = w;
            kp.new_h = h;
            set_doc(d);
        } else {
            message(GTK_MESSAGE_ERROR, "%s", _("Not enough memory for an image of that size."));
        }
    }
    gtk_widget_destroy(dlg);
}

static void open_cb(void)
{
    if (!confirm_discard())
        return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Open image"), GTK_WINDOW(kp.win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OPEN,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, _("Images"));
    gtk_file_filter_add_pixbuf_formats(f);
    gtk_file_filter_add_pattern(f, "*.ora");
    gtk_file_filter_add_pattern(f, "*.ORA");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, _("All files"));
    gtk_file_filter_add_pattern(f, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    if (kp.doc->path) {
        char *dir = g_path_get_dirname(kp.doc->path);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir);
        g_free(dir);
    }
    char *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);
    if (path)
        kp_open_path(path);
    g_free(path);
}

static void save_cb(void)
{
    save();
}

static void save_as_cb(void)
{
    save_as();
}

static void quit_cb(void)
{
    if (confirm_discard())
        gtk_widget_destroy(kp.win);
}

static void undo_cb(void)
{
    kp_undo_undo();
}

static void redo_cb(void)
{
    kp_undo_redo();
}

static void zoom_in_cb(void)
{
    kp_canvas_zoom_step(1);
}

static void zoom_out_cb(void)
{
    kp_canvas_zoom_step(-1);
}

static void zoom_100_cb(void)
{
    kp_canvas_zoom_100();
}

static void zoom_fit_cb(void)
{
    kp_canvas_zoom_fit();
}

static void grid_cb(GtkCheckMenuItem *item)
{
    kp.grid = gtk_check_menu_item_get_active(item);
    kp_canvas_redraw();
}

static void panel_cb(GtkCheckMenuItem *item)
{
    kp_layers_set_shown(gtk_check_menu_item_get_active(item));
}

static void about_cb(void)
{
    gtk_show_about_dialog(GTK_WINDOW(kp.win), "program-name", "kipaint", "version", KIPAINT_VERSION, "comments",
                          _("A simple paint program"), "logo-icon-name", "kipaint", NULL);
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    return !confirm_discard();
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
    menu_item(m, ag, _("_New..."), GDK_n, C, G_CALLBACK(new_cb));
    menu_item(m, ag, _("_Open..."), GDK_o, C, G_CALLBACK(open_cb));
    separator(m);
    menu_item(m, ag, _("_Save"), GDK_s, C, G_CALLBACK(save_cb));
    menu_item(m, ag, _("Save _as..."), GDK_s, CS, G_CALLBACK(save_as_cb));
    g_export_item = menu_item(m, ag, _("_Export..."), GDK_e, CS, G_CALLBACK(export_cb));
    gtk_widget_set_no_show_all(g_export_item, TRUE);
    separator(m);
    menu_item(m, ag, _("_Quit"), GDK_q, C, G_CALLBACK(quit_cb));

    m = submenu(bar, _("_Edit"));
    g_undo_item = menu_item(m, ag, _("_Undo"), GDK_z, C, G_CALLBACK(undo_cb));
    g_redo_item = menu_item(m, ag, _("_Redo"), GDK_y, C, G_CALLBACK(redo_cb));

    m = submenu(bar, _("_View"));
    menu_item(m, ag, _("Zoom _in"), GDK_plus, C, G_CALLBACK(zoom_in_cb));
    menu_item(m, ag, _("Zoom _out"), GDK_minus, C, G_CALLBACK(zoom_out_cb));
    menu_item(m, ag, _("_Actual size"), GDK_0, C, G_CALLBACK(zoom_100_cb));
    menu_item(m, ag, _("_Fit to window"), GDK_0, CS, G_CALLBACK(zoom_fit_cb));
    separator(m);
    g_grid_item = gtk_check_menu_item_new_with_mnemonic(_("Pixel _grid"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_grid_item), kp.grid);
    gtk_widget_add_accelerator(g_grid_item, "activate", ag, GDK_g, C, GTK_ACCEL_VISIBLE);
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(g_grid_item, _("Shown from 800% zoom up"));
    g_signal_connect(g_grid_item, "toggled", G_CALLBACK(grid_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), g_grid_item);
    g_panel_item = gtk_check_menu_item_new_with_mnemonic(_("_Layers panel"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_panel_item), kp.layers_panel);
    g_signal_connect(g_panel_item, "toggled", G_CALLBACK(panel_cb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), g_panel_item);

    m = submenu(bar, _("_Layers"));
    menu_item(m, ag, _("_New layer"), GDK_n, CS, G_CALLBACK(kp_layers_add));
    menu_item(m, ag, _("_Duplicate layer"), GDK_d, CS, G_CALLBACK(kp_layers_duplicate));
    menu_item(m, ag, _("De_lete layer"), 0, 0, G_CALLBACK(kp_layers_delete));
    separator(m);
    menu_item(m, ag, _("_Raise layer"), GDK_Page_Up, C, G_CALLBACK(kp_layers_raise));
    menu_item(m, ag, _("Lo_wer layer"), GDK_Page_Down, C, G_CALLBACK(kp_layers_lower));

    m = submenu(bar, _("_Help"));
    menu_item(m, ag, _("_About"), 0, 0, G_CALLBACK(about_cb));
    return bar;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)data;
    guint mods = ev->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    /* Ctrl+= is Ctrl+plus on keyboards where plus needs Shift. */
    if ((mods & GDK_CONTROL_MASK) && (ev->keyval == GDK_equal || ev->keyval == GDK_KP_Add)) {
        kp_canvas_zoom_step(1);
        return TRUE;
    }
    if ((mods & GDK_CONTROL_MASK) && ev->keyval == GDK_KP_Subtract) {
        kp_canvas_zoom_step(-1);
        return TRUE;
    }
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && (ev->keyval == GDK_Z || ev->keyval == GDK_z)) {
        kp_undo_redo();
        return TRUE;
    }
    /* Single keys, unless something is being typed (the size spin). */
    if (mods & ~GDK_SHIFT_MASK)
        return FALSE;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(w));
    if (focus && GTK_IS_ENTRY(focus))
        return FALSE;
    guint key = gdk_keyval_to_lower(ev->keyval);
    for (int i = 0; i < kp_n_tools; i++)
        if (kp_tools[i]->key == key) {
            kp_toolbox_select(kp_tools[i]);
            return TRUE;
        }
    if (key == GDK_bracketleft || key == GDK_bracketright) {
        kp_tool_size_step(key == GDK_bracketright ? 1 : -1);
        return TRUE;
    }
    if (key == GDK_x) {
        KpColor c = kp.color[0];
        kp.color[0] = kp.color[1];
        kp.color[1] = c;
        kp_palette_refresh();
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *status_label(GtkWidget *box, int width_chars)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_width_chars(GTK_LABEL(l), width_chars);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_misc_set_padding(GTK_MISC(l), 8, 2);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_vseparator_new(), FALSE, FALSE, 0);
    return l;
}

int main(int argc, char **argv)
{
    xis_i18n_init("kipaint");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("kipaint %s\n", KIPAINT_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("kipaint %s - simple paint program\n\nUsage: kipaint [IMAGE]\n", KIPAINT_VERSION);
            return 0;
        }
    }
    gtk_init(&argc, &argv);
    kp_config_load();
    kp.tool = &kp_tool_pencil;
    gtk_window_set_default_icon_name("kipaint");

    kp.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(kp.win), 900, 650);
    g_signal_connect(kp.win, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(kp.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(kp.win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(kp.win), ag);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(kp.win), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menubar(ag), FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), kp_toolbox_new(), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(frame), kp_canvas_new());
    gtk_box_pack_start(GTK_BOX(hbox), frame, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), kp_layers_panel_new(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), kp_palette_new(), FALSE, FALSE, 0);

    GtkWidget *status = gtk_hbox_new(FALSE, 0);
    g_pos_label = status_label(status, 14);
    g_size_label = status_label(status, 16);
    g_zoom_label = status_label(status, 6);
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 0);

    const char *file = NULL;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            file = argv[i];
    if (!file || !kp_open_path(file)) {
        KpColor white = { 1, 1, 1, 1 };
        set_doc(kp_doc_new(kp.new_w, kp.new_h, &white));
    }

    gtk_widget_show_all(kp.win);
    kp_toolbox_select(kp.tool);
    kp_canvas_grab_focus();
    gtk_main();
    kp_config_save();
    return 0;
}
