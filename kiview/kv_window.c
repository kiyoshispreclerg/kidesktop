/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The window: the area where the plugin draws, the footer with its buttons,
 * and everything the user does, turned into messages for the plugin.
 *
 * The plugin draws into an X window of ours, a child of the area's own
 * window that GDK doesn't know about (like Kiplay's video window). It
 * selects no input, so clicks and the wheel go on to the area. */
#include <X11/Xlib.h>
#include <fcntl.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "../shared/xis_i18n.h"
#include "../shared/xis_winstate.h"
#include "kv.h"

#define ZOOM_STEP 1.25
#define SCROLL_STEP 80.0
#define OPEN_TIMEOUT_SECONDS 10
#define BG_NORMAL "#1e1e1e"
#define BG_FULLSCREEN "#000000"

static struct {
    GtkWidget *win, *area, *footer, *zoom_lbl, *pos_lbl;
    GtkToolItem *prev, *next;
    KvFolder *folder;
    KvProc *proc;
    Window xwin;
    int aw, ah;
    long seq;
    long next_id, cur_id;
    guint open_timeout;
    char *message; /* shown in the area instead of a document */
    char *format;
    double zoom;
    gboolean scroll_x, scroll_y, fit;
    gboolean fullscreen, dragging, started;
    double lx, ly;
    GFile *pending; /* what to open once realized */
} kv;

static Display *dpy(void)
{
    return GDK_WINDOW_XDISPLAY(kv.area->window);
}

/* ---- sending ---- */

static void to_plugin(GString *m)
{
    if (kv.proc)
        kv_proc_send(kv.proc, m, -1);
    else
        g_string_free(m, TRUE);
}

static void send_surface(void)
{
    if (!kv.proc || !kv.xwin || kv.aw < 1 || kv.ah < 1)
        return;
    GString *m = kvp_out("surface");
    kvp_out_i(m, "xid", (long)kv.xwin);
    kvp_out_i(m, "w", kv.aw);
    kvp_out_i(m, "h", kv.ah);
    kvp_out_s(m, "density", "1/1");
    kvp_out_i(m, "seq", ++kv.seq);
    to_plugin(m);
}

static void send_style(void)
{
    GString *m = kvp_out("style");
    kvp_out_s(m, "bg", kv.fullscreen ? BG_FULLSCREEN : BG_NORMAL);
    kvp_out_i(m, "checker", 1);
    to_plugin(m);
}

static void send_zoom(const char *mode, double factor, double x, double y)
{
    GString *m = kvp_out("zoom");
    kvp_out_s(m, "mode", mode);
    if (factor > 0)
        kvp_out_d(m, "factor", factor);
    if (x >= 0) {
        kvp_out_d(m, "x", x);
        kvp_out_d(m, "y", y);
    }
    to_plugin(m);
}

static void zoom_centered(double factor)
{
    send_zoom("by", factor, kv.aw / 2.0, kv.ah / 2.0);
}

static void send_pan(double dx, double dy)
{
    GString *m = kvp_out("pan");
    kvp_out_d(m, "dx", dx);
    kvp_out_d(m, "dy", dy);
    to_plugin(m);
}

static void send_rotate(int by)
{
    GString *m = kvp_out("rotate");
    kvp_out_i(m, "by", by);
    to_plugin(m);
}

/* ---- state shown ---- */

static void update_labels(void)
{
    char *t;
    if (kv.cur_id && !kv.message) {
        t = g_strdup_printf("%d%%", (int)round(kv.zoom * 100));
        gtk_label_set_text(GTK_LABEL(kv.zoom_lbl), t);
        g_free(t);
    } else {
        gtk_label_set_text(GTK_LABEL(kv.zoom_lbl), "");
    }
    int n = kv.folder ? kv_folder_count(kv.folder) : 0, pos = kv.folder ? kv_folder_pos(kv.folder) : -1;
    if (n > 0 && pos >= 0) {
        /* TRANSLATORS: position in the folder, e.g. "3 of 12" */
        t = g_strdup_printf(_("%d of %d"), pos + 1, n);
        gtk_label_set_text(GTK_LABEL(kv.pos_lbl), t);
        g_free(t);
    } else {
        gtk_label_set_text(GTK_LABEL(kv.pos_lbl), "");
    }
    gtk_widget_set_sensitive(GTK_WIDGET(kv.prev), n > 1);
    gtk_widget_set_sensitive(GTK_WIDGET(kv.next), n > 1);

    const char *name = kv.folder ? kv_folder_current(kv.folder) : NULL;
    t = name ? g_strdup_printf("%s — kiview", name) : g_strdup("kiview");
    gtk_window_set_title(GTK_WINDOW(kv.win), t);
    g_free(t);
}

static void set_message(const char *msg)
{
    g_free(kv.message);
    kv.message = g_strdup(msg);
    if (kv.xwin) {
        if (msg)
            XUnmapWindow(dpy(), kv.xwin);
        else
            XMapWindow(dpy(), kv.xwin);
        XFlush(dpy());
    }
    gtk_widget_queue_draw(kv.area);
    update_labels();
}

static void cancel_open_timeout(void)
{
    if (kv.open_timeout)
        g_source_remove(kv.open_timeout);
    kv.open_timeout = 0;
}

static void drop_proc(void)
{
    if (kv.proc)
        kv_proc_stop(kv.proc);
    kv.proc = NULL;
    cancel_open_timeout();
}

/* ---- plugin messages ---- */

static const char *failure_text(const char *reason)
{
    if (!strcmp(reason, "unsupported"))
        return _("This kind of image is not supported.");
    if (!strcmp(reason, "toolarge"))
        return _("This file is too large to show.");
    if (!strcmp(reason, "corrupt"))
        return _("This file is damaged or incomplete.");
    return _("This file could not be read.");
}

static void on_plugin(KvProc *p, const KvpMsg *m, gpointer ud)
{
    (void)ud;
    if (!m) {
        kv_proc_stop(p);
        if (kv.proc == p) {
            kv.proc = NULL;
            cancel_open_timeout();
            set_message(_("The viewer stopped unexpectedly. Pick another file to try again."));
        }
        return;
    }
    if (p != kv.proc)
        return;
    const char *v = m->verb;
    if (!strcmp(v, "opened") && kvp_msg_get_int(m, "id", 0) == kv.cur_id) {
        cancel_open_timeout();
        g_free(kv.format);
        kv.format = g_strdup(kvp_msg_get(m, "format"));
        set_message(NULL);
    } else if (!strcmp(v, "failed") && kvp_msg_get_int(m, "id", 0) == kv.cur_id) {
        cancel_open_timeout();
        set_message(failure_text(kvp_msg_get(m, "reason") ? kvp_msg_get(m, "reason") : ""));
    } else if (!strcmp(v, "view") && kvp_msg_get_int(m, "id", 0) == kv.cur_id) {
        kv.zoom = kvp_msg_get_double(m, "zoom", 1);
        kv.fit = kvp_msg_get_int(m, "fit", 0) != 0;
        kv.scroll_x = kvp_msg_get_int(m, "scroll_x", 0) != 0;
        kv.scroll_y = kvp_msg_get_int(m, "scroll_y", 0) != 0;
        update_labels();
    }
}

static gboolean open_timed_out(gpointer data)
{
    (void)data;
    kv.open_timeout = 0;
    drop_proc();
    set_message(_("The viewer did not answer. Pick another file to try again."));
    return G_SOURCE_REMOVE;
}

/* ---- opening ---- */

static void show_current(void)
{
    const char *name = kv.folder ? kv_folder_current(kv.folder) : NULL;
    kv.zoom = 1;
    kv.scroll_x = kv.scroll_y = FALSE;
    update_labels();
    if (!name) {
        set_message(_("Nothing to show. Use Ctrl+O to open an image."));
        return;
    }
    char *path = g_build_filename(g_file_peek_path(kv_folder_dir(kv.folder)), name, NULL);
    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(file, "standard::content-type", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    const char *mime = info ? g_file_info_get_content_type(info) : NULL;
    const KvManifest *man = kv_plugins_for(mime, name);
    if (!man) {
        set_message(_("There is no viewer for this kind of file."));
        goto out;
    }
    if (kv.proc && kv_proc_manifest(kv.proc) != man)
        drop_proc();
    if (!kv.proc) {
        kv.proc = kv_proc_start(man, on_plugin, NULL);
        if (!kv.proc) {
            set_message(_("The viewer could not be started."));
            goto out;
        }
        GString *h = kvp_out("hello");
        kvp_out_i(h, "version", 1);
        to_plugin(h);
        send_surface();
        send_style();
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_message(failure_text("io"));
        goto out;
    }
    GString *m = kvp_out("open");
    kvp_out_i(m, "id", ++kv.next_id);
    kvp_out_s(m, "name", name);
    kvp_out_s(m, "mime", mime ? mime : "");
    kv.cur_id = kv.next_id;
    kv_proc_send(kv.proc, m, fd);
    close(fd);
    cancel_open_timeout();
    kv.open_timeout = g_timeout_add_seconds(OPEN_TIMEOUT_SECONDS, open_timed_out, NULL);
out:
    if (info)
        g_object_unref(info);
    g_object_unref(file);
    g_free(path);
}

static void step(int delta)
{
    if (!kv.folder || kv_folder_count(kv.folder) < 2)
        return;
    kv_folder_step(kv.folder, delta);
    show_current();
}

static void goto_index(int index)
{
    if (!kv.folder || kv_folder_count(kv.folder) < 1)
        return;
    kv_folder_goto(kv.folder, index);
    show_current();
}

static void folder_changed(gpointer ud)
{
    (void)ud;
    update_labels();
}

static void open_file(GFile *file)
{
    GFileInfo *info = g_file_query_info(file, "standard::type", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    gboolean is_dir = info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    if (info)
        g_object_unref(info);
    GFile *dir = is_dir ? g_object_ref(file) : g_file_get_parent(file);
    char *base = is_dir ? NULL : g_file_get_basename(file);
    if (!dir) {
        g_free(base);
        return;
    }
    kv_folder_free(kv.folder);
    kv.folder = kv_folder_new(dir, base, folder_changed, NULL);
    g_object_unref(dir);
    g_free(base);
    show_current();
}

static void choose_file(void)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Open"), GTK_WINDOW(kv.win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OPEN,
                                                 GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        if (f) {
            open_file(f);
            g_object_unref(f);
        }
    }
    gtk_widget_destroy(dlg);
}

/* ---- window ---- */

static void set_fullscreen(gboolean on)
{
    if (on)
        gtk_window_fullscreen(GTK_WINDOW(kv.win));
    else
        gtk_window_unfullscreen(GTK_WINDOW(kv.win));
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer ud)
{
    (void)w;
    (void)ud;
    if (e->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
        kv.fullscreen = (e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
        gtk_widget_set_visible(kv.footer, !kv.fullscreen);
        send_style();
    }
    return FALSE;
}

static void on_realize(GtkWidget *area, gpointer ud)
{
    (void)ud;
    Display *d = GDK_WINDOW_XDISPLAY(area->window);
    kv.aw = MAX(area->allocation.width, 1);
    kv.ah = MAX(area->allocation.height, 1);
    kv.xwin = XCreateSimpleWindow(d, GDK_WINDOW_XID(area->window), 0, 0, (unsigned)kv.aw, (unsigned)kv.ah,
                                  0, 0, 0x1e1e1e);
    if (!kv.message)
        XMapWindow(d, kv.xwin);
    XFlush(d);
}

static void on_unrealize(GtkWidget *area, gpointer ud)
{
    (void)area;
    (void)ud;
    kv.xwin = 0; /* the server destroys it with the area's window */
}

static void on_allocate(GtkWidget *area, GtkAllocation *a, gpointer ud)
{
    (void)area;
    (void)ud;
    if (!kv.xwin)
        return;
    kv.aw = MAX(a->width, 1);
    kv.ah = MAX(a->height, 1);
    XMoveResizeWindow(dpy(), kv.xwin, 0, 0, (unsigned)kv.aw, (unsigned)kv.ah);
    XFlush(dpy());
    send_surface();
}

static gboolean on_expose(GtkWidget *area, GdkEventExpose *e, gpointer ud)
{
    (void)e;
    (void)ud;
    cairo_t *cr = gdk_cairo_create(area->window);
    GdkColor c;
    gdk_color_parse(kv.fullscreen ? BG_FULLSCREEN : BG_NORMAL, &c);
    gdk_cairo_set_source_color(cr, &c);
    cairo_paint(cr);
    if (kv.message) {
        PangoLayout *l = gtk_widget_create_pango_layout(area, kv.message);
        pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
        pango_layout_set_width(l, MAX(area->allocation.width - 40, 100) * PANGO_SCALE);
        int w, h;
        pango_layout_get_pixel_size(l, &w, &h);
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_move_to(cr, (area->allocation.width - MAX(area->allocation.width - 40, 100)) / 2.0 + 0,
                      (area->allocation.height - h) / 2.0);
        pango_cairo_show_layout(cr, l);
        g_object_unref(l);
    }
    cairo_destroy(cr);
    return TRUE;
}

static void update_cursor(void)
{
    GdkCursor *c = NULL;
    if (kv.dragging)
        c = gdk_cursor_new(GDK_FLEUR);
    else if (kv.scroll_x || kv.scroll_y)
        c = gdk_cursor_new(GDK_HAND1);
    gdk_window_set_cursor(kv.area->window, c);
    if (c)
        gdk_cursor_unref(c);
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *e, gpointer ud)
{
    (void)w;
    (void)ud;
    if (e->button == 1 && e->type == GDK_2BUTTON_PRESS) {
        set_fullscreen(!kv.fullscreen);
        return TRUE;
    }
    if (e->button == 1 && e->type == GDK_BUTTON_PRESS) {
        kv.dragging = kv.scroll_x || kv.scroll_y;
        kv.lx = e->x;
        kv.ly = e->y;
        update_cursor();
    }
    return TRUE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *e, gpointer ud)
{
    (void)w;
    (void)ud;
    if (e->button == 1) {
        kv.dragging = FALSE;
        update_cursor();
    }
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer ud)
{
    (void)w;
    (void)ud;
    if (kv.dragging) {
        send_pan(kv.lx - e->x, kv.ly - e->y);
        kv.lx = e->x;
        kv.ly = e->y;
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer ud)
{
    (void)w;
    (void)ud;
    int dir = e->direction == GDK_SCROLL_UP || e->direction == GDK_SCROLL_LEFT ? -1 : 1;
    gboolean horizontal = e->direction == GDK_SCROLL_LEFT || e->direction == GDK_SCROLL_RIGHT ||
                          (e->state & GDK_SHIFT_MASK);
    if (e->state & GDK_CONTROL_MASK) {
        send_zoom("by", dir < 0 ? ZOOM_STEP : 1 / ZOOM_STEP, e->x, e->y);
    } else if (kv.scroll_x || kv.scroll_y) {
        if (horizontal)
            send_pan(dir * SCROLL_STEP, 0);
        else
            send_pan(0, dir * SCROLL_STEP);
    } else {
        step(dir);
    }
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer ud)
{
    (void)w;
    (void)ud;
    gboolean ctrl = (e->state & GDK_CONTROL_MASK) != 0;
    switch (e->keyval) {
    case GDK_Left: case GDK_Page_Up: case GDK_BackSpace:
        step(-1);
        return TRUE;
    case GDK_Right: case GDK_Page_Down: case GDK_space:
        step(1);
        return TRUE;
    case GDK_Home:
        goto_index(0);
        return TRUE;
    case GDK_End:
        goto_index(kv.folder ? kv_folder_count(kv.folder) - 1 : 0);
        return TRUE;
    case GDK_Up:
        send_pan(0, -SCROLL_STEP);
        return TRUE;
    case GDK_Down:
        send_pan(0, SCROLL_STEP);
        return TRUE;
    case GDK_f: case GDK_F: case GDK_KP_Multiply:
        send_zoom("fit", 0, -1, -1);
        return TRUE;
    case GDK_1: case GDK_KP_Divide:
        send_zoom("actual", 0, -1, -1);
        return TRUE;
    case GDK_plus: case GDK_equal: case GDK_KP_Add:
        zoom_centered(ZOOM_STEP);
        return TRUE;
    case GDK_minus: case GDK_KP_Subtract:
        zoom_centered(1 / ZOOM_STEP);
        return TRUE;
    case GDK_comma:
        if (ctrl) {
            send_rotate(-90);
            return TRUE;
        }
        break;
    case GDK_period:
        if (ctrl) {
            send_rotate(90);
            return TRUE;
        }
        break;
    case GDK_F11:
        set_fullscreen(!kv.fullscreen);
        return TRUE;
    case GDK_Escape:
        if (kv.fullscreen) {
            set_fullscreen(FALSE);
            return TRUE;
        }
        break;
    case GDK_o:
        if (ctrl) {
            choose_file();
            return TRUE;
        }
        break;
    case GDK_q: case GDK_w:
        if (ctrl) {
            gtk_main_quit();
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- footer ---- */

static void on_prev(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; step(-1); }
static void on_next(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; step(1); }
static void on_fit(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; send_zoom("fit", 0, -1, -1); }
static void on_actual(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; send_zoom("actual", 0, -1, -1); }
static void on_zoom_in(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; zoom_centered(ZOOM_STEP); }
static void on_zoom_out(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; zoom_centered(1 / ZOOM_STEP); }
static void on_full(GtkToolButton *b, gpointer ud) { (void)b; (void)ud; set_fullscreen(TRUE); }

static GtkToolItem *add_button(GtkWidget *bar, const char *stock, const char *tip, GCallback cb)
{
    GtkToolItem *it = gtk_tool_button_new_from_stock(stock);
    gtk_tool_item_set_tooltip_text(it, tip);
    g_signal_connect(it, "clicked", cb, NULL);
    gtk_toolbar_insert(GTK_TOOLBAR(bar), it, -1);
    return it;
}

static GtkWidget *build_footer(void)
{
    GtkWidget *bar = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_show_arrow(GTK_TOOLBAR(bar), FALSE);
    kv.prev = add_button(bar, GTK_STOCK_GO_BACK, _("Previous image (Left)"), G_CALLBACK(on_prev));
    kv.next = add_button(bar, GTK_STOCK_GO_FORWARD, _("Next image (Right)"), G_CALLBACK(on_next));
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    add_button(bar, GTK_STOCK_ZOOM_FIT, _("Best fit (F)"), G_CALLBACK(on_fit));
    add_button(bar, GTK_STOCK_ZOOM_100, _("Actual size (1)"), G_CALLBACK(on_actual));
    add_button(bar, GTK_STOCK_ZOOM_OUT, _("Zoom out (-, Ctrl+wheel)"), G_CALLBACK(on_zoom_out));
    add_button(bar, GTK_STOCK_ZOOM_IN, _("Zoom in (+, Ctrl+wheel)"), G_CALLBACK(on_zoom_in));
    gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);
    add_button(bar, GTK_STOCK_FULLSCREEN, _("Full screen (F11)"), G_CALLBACK(on_full));

    kv.pos_lbl = gtk_label_new("");
    kv.zoom_lbl = gtk_label_new("");
    gtk_widget_set_size_request(kv.pos_lbl, 90, -1);
    gtk_widget_set_size_request(kv.zoom_lbl, 90, -1);
    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(row), kv.pos_lbl, FALSE, FALSE, 6);
    GtkWidget *mid = gtk_alignment_new(0.5, 0.5, 0, 0);
    gtk_container_add(GTK_CONTAINER(mid), bar);
    gtk_box_pack_start(GTK_BOX(row), mid, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), kv.zoom_lbl, FALSE, FALSE, 6);
    return row;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer ud)
{
    (void)w;
    (void)e;
    (void)ud;
    gtk_main_quit();
    return TRUE;
}

static gboolean start_idle(gpointer ud)
{
    (void)ud;
    if (kv.pending) {
        open_file(kv.pending);
        g_object_unref(kv.pending);
        kv.pending = NULL;
    } else {
        show_current();
    }
    return G_SOURCE_REMOVE;
}

void kv_window_run(GFile *file)
{
    kv_plugins_load();
    kv.pending = file ? g_object_ref(file) : NULL;

    kv.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(kv.win), 900, 650);
    gtk_window_set_icon_name(GTK_WINDOW(kv.win), "kiview");
    xis_winstate_restore(GTK_WINDOW(kv.win), "kiview", XIS_WINSTATE_ALL);
    g_signal_connect(kv.win, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(kv.win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(kv.win, "window-state-event", G_CALLBACK(on_window_state), NULL);

    kv.area = gtk_drawing_area_new();
    gtk_widget_set_double_buffered(kv.area, FALSE);
    gtk_widget_add_events(kv.area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                       GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    g_signal_connect(kv.area, "realize", G_CALLBACK(on_realize), NULL);
    g_signal_connect(kv.area, "unrealize", G_CALLBACK(on_unrealize), NULL);
    g_signal_connect(kv.area, "size-allocate", G_CALLBACK(on_allocate), NULL);
    g_signal_connect(kv.area, "expose-event", G_CALLBACK(on_expose), NULL);
    g_signal_connect(kv.area, "button-press-event", G_CALLBACK(on_button_press), NULL);
    g_signal_connect(kv.area, "button-release-event", G_CALLBACK(on_button_release), NULL);
    g_signal_connect(kv.area, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    g_signal_connect(kv.area, "scroll-event", G_CALLBACK(on_scroll), NULL);

    kv.footer = build_footer();
    GtkWidget *box = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), kv.area, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), kv.footer, FALSE, FALSE, 2);
    gtk_container_add(GTK_CONTAINER(kv.win), box);
    gtk_widget_show_all(kv.win);
    xis_winstate_track(GTK_WINDOW(kv.win), "kiview", XIS_WINSTATE_ALL);
    update_labels();

    g_idle_add(start_idle, NULL);
    gtk_main();

    drop_proc();
    kv_folder_free(kv.folder);
}
