/* ks_region.c - rectangular region selection over a frozen screen.
 *
 * A popup the size of the whole root shows the screenshot taken just
 * before, dimmed; dragging uncovers the chosen rectangle. Releasing the
 * button accepts, Esc or the right button cancels. The screen is frozen
 * on purpose, so a menu or a tooltip can be captured while it is open. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkkeysyms.h>

typedef struct {
    GtkWidget *win;
    GdkPixbuf *frozen;
    KsRegionDone done;
    void *data;
    gboolean dragging;
    int x0, y0, x1, y1;
} Region;

static void norm(const Region *r, KsRect *out)
{
    out->x = MIN(r->x0, r->x1);
    out->y = MIN(r->y0, r->y1);
    out->w = ABS(r->x1 - r->x0);
    out->h = ABS(r->y1 - r->y0);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, Region *r)
{
    (void)ev;
    cairo_t *cr = gdk_cairo_create(w->window);
    gdk_cairo_set_source_pixbuf(cr, r->frozen, 0, 0);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_paint(cr);
    if (r->dragging || r->x0 != r->x1) {
        KsRect s;
        norm(r, &s);
        cairo_save(cr);
        cairo_rectangle(cr, s.x, s.y, s.w, s.h);
        cairo_clip(cr);
        gdk_cairo_set_source_pixbuf(cr, r->frozen, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_set_source_rgb(cr, 0.24, 0.6, 0.95);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, s.x + 0.5, s.y + 0.5, MAX(s.w - 1, 0), MAX(s.h - 1, 0));
        cairo_stroke(cr);

        char *txt = g_strdup_printf("%d × %d", s.w, s.h);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13);
        cairo_text_extents_t te;
        cairo_text_extents(cr, txt, &te);
        double tx = s.x, ty = s.y > 24 ? s.y - 8 : s.y + s.h + 18;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_rectangle(cr, tx - 4, ty - te.height - 4, te.x_advance + 8, te.height + 8);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, tx, ty);
        cairo_show_text(cr, txt);
        g_free(txt);
    } else {
        const char *hint = _("Drag to select a region. Esc cancels.");
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 16);
        cairo_text_extents_t te;
        cairo_text_extents(cr, hint, &te);
        int px, py;
        gdk_display_get_pointer(gdk_display_get_default(), NULL, &px, &py, NULL);
        GdkScreen *scr = gdk_screen_get_default();
        GdkRectangle g;
        gdk_screen_get_monitor_geometry(scr, gdk_screen_get_monitor_at_point(scr, px, py), &g);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, g.x + (g.width - te.width) / 2, g.y + g.height / 2);
        cairo_show_text(cr, hint);
    }
    cairo_destroy(cr);
    return TRUE;
}

static void finish(Region *r, gboolean ok)
{
    KsRect s;
    norm(r, &s);
    gdk_keyboard_ungrab(GDK_CURRENT_TIME);
    gdk_pointer_ungrab(GDK_CURRENT_TIME);
    gtk_widget_destroy(r->win);
    gdk_flush();
    if (ok && (s.w < 2 || s.h < 2))
        ok = FALSE;
    r->done(ok, &s, r->data);
    g_object_unref(r->frozen);
    g_free(r);
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, Region *r)
{
    (void)w;
    if (ev->button == 3) {
        finish(r, FALSE);
        return TRUE;
    }
    if (ev->button != 1)
        return FALSE;
    r->dragging = TRUE;
    r->x0 = r->x1 = (int)ev->x_root;
    r->y0 = r->y1 = (int)ev->y_root;
    gtk_widget_queue_draw(r->win);
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, Region *r)
{
    (void)w;
    if (r->dragging) {
        r->x1 = (int)ev->x_root;
        r->y1 = (int)ev->y_root;
    }
    gtk_widget_queue_draw(r->win);
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, Region *r)
{
    (void)w;
    if (ev->button != 1 || !r->dragging)
        return FALSE;
    r->x1 = (int)ev->x_root;
    r->y1 = (int)ev->y_root;
    r->dragging = FALSE;
    finish(r, TRUE);
    return TRUE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, Region *r)
{
    (void)w;
    if (ev->keyval == GDK_Escape) {
        finish(r, FALSE);
        return TRUE;
    }
    return FALSE;
}

static gboolean grab_later(gpointer data)
{
    Region *r = data;
    GdkWindow *gw = r->win->window;
    GdkCursor *cur = gdk_cursor_new(GDK_CROSSHAIR);
    gdk_pointer_grab(gw, FALSE, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK,
                     NULL, cur, GDK_CURRENT_TIME);
    gdk_keyboard_grab(gw, FALSE, GDK_CURRENT_TIME);
    gdk_cursor_unref(cur);
    return FALSE;
}

void ks_region_select(GdkPixbuf *frozen, KsRegionDone done, void *data)
{
    Region *r = g_new0(Region, 1);
    r->frozen = g_object_ref(frozen);
    r->done = done;
    r->data = data;
    r->win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_move(GTK_WINDOW(r->win), 0, 0);
    gtk_window_resize(GTK_WINDOW(r->win), gdk_pixbuf_get_width(frozen), gdk_pixbuf_get_height(frozen));
    gtk_widget_set_app_paintable(r->win, TRUE);
    gtk_widget_set_double_buffered(r->win, TRUE);
    gtk_widget_add_events(r->win, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                      GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(r->win, "expose-event", G_CALLBACK(on_expose), r);
    g_signal_connect(r->win, "button-press-event", G_CALLBACK(on_press), r);
    g_signal_connect(r->win, "button-release-event", G_CALLBACK(on_release), r);
    g_signal_connect(r->win, "motion-notify-event", G_CALLBACK(on_motion), r);
    g_signal_connect(r->win, "key-press-event", G_CALLBACK(on_key), r);
    gtk_widget_show(r->win);
    g_idle_add(grab_later, r);
}
