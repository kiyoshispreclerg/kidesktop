/* kfd_date.c - a date entry with a calendar popup.
 *
 * The text is free (the locale's date format, or YYYY-MM-DD); the icon at
 * the end opens a calendar under the entry, and picking a day writes it in
 * the locale's format. */
#include "kifind.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkkeysyms.h>
#include <stdio.h>

typedef struct {
    GtkWidget *entry, *win, *cal;
    gboolean month_moved;       /* the next day-selected comes from a month change */
    guint moved_idle;
} Popup;

static gboolean parse(const char *text, GDate *d)
{
    int y, m, day;
    char tail;
    g_date_clear(d, 1);
    if (sscanf(text, "%d-%d-%d%c", &y, &m, &day, &tail) == 3) {
        if (g_date_valid_dmy(day, m, y))
            g_date_set_dmy(d, day, m, y);
    } else {
        g_date_set_parse(d, text);
    }
    return g_date_valid(d);
}

gboolean kfd_date_get(GtkWidget *e, gboolean end_of_day, gint64 *out)
{
    char *text = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
    *out = KFD_NO_LIMIT;
    if (!*text) {
        g_free(text);
        return TRUE;
    }
    GDate d;
    gboolean ok = parse(text, &d);
    g_free(text);
    if (!ok)
        return FALSE;
    GDateTime *dt = g_date_time_new_local(g_date_get_year(&d), g_date_get_month(&d), g_date_get_day(&d), 0, 0, 0);
    if (!dt)
        return FALSE;
    *out = g_date_time_to_unix(dt) + (end_of_day ? 24 * 3600 - 1 : 0);
    g_date_time_unref(dt);
    return TRUE;
}

static void popup_close(Popup *p)
{
    if (p->moved_idle)
        g_source_remove(p->moved_idle);
    gdk_display_pointer_ungrab(gtk_widget_get_display(p->win), GDK_CURRENT_TIME);
    gdk_display_keyboard_ungrab(gtk_widget_get_display(p->win), GDK_CURRENT_TIME);
    gtk_grab_remove(p->win);
    gtk_widget_destroy(p->win);
    g_free(p);
}

static gboolean clear_moved(gpointer d)
{
    Popup *p = d;
    p->month_moved = FALSE;
    p->moved_idle = 0;
    return FALSE;
}

/* Changing the month can re-emit day-selected; that is not a pick. */
static void on_month_changed(GtkCalendar *c, gpointer d)
{
    (void)c;
    Popup *p = d;
    p->month_moved = TRUE;
    if (!p->moved_idle)
        p->moved_idle = g_idle_add(clear_moved, p);
}

static void on_day_selected(GtkCalendar *c, gpointer d)
{
    Popup *p = d;
    if (p->month_moved)
        return;
    guint y, m, day;
    gtk_calendar_get_date(c, &y, &m, &day);
    if (!day)
        return;
    GDate *g = g_date_new_dmy(day, m + 1, y);
    char buf[64];
    g_date_strftime(buf, sizeof(buf), "%x", g);
    g_date_free(g);
    gtk_entry_set_text(GTK_ENTRY(p->entry), buf);
    popup_close(p);
}

static void on_clear(GtkButton *b, gpointer d)
{
    (void)b;
    Popup *p = d;
    gtk_entry_set_text(GTK_ENTRY(p->entry), "");
    popup_close(p);
}

/* With the grab, a press anywhere comes here; outside the popup closes it. */
static gboolean on_popup_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    int x, y;
    GtkAllocation a;
    gdk_window_get_origin(gtk_widget_get_window(w), &x, &y);
    gtk_widget_get_allocation(w, &a);
    if (ev->x_root < x || ev->y_root < y || ev->x_root >= x + a.width || ev->y_root >= y + a.height) {
        popup_close(d);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_popup_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    if (ev->keyval == GDK_KEY_Escape) {
        popup_close(d);
        return TRUE;
    }
    return FALSE;
}

static void on_icon_press(GtkEntry *e, GtkEntryIconPosition pos, GdkEvent *ev, gpointer d)
{
    (void)pos; (void)d;
    Popup *p = g_new0(Popup, 1);
    p->entry = GTK_WIDGET(e);
    p->win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_screen(GTK_WINDOW(p->win), gtk_widget_get_screen(p->entry));
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    gtk_container_add(GTK_CONTAINER(p->win), frame);
    GtkWidget *box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    gtk_container_add(GTK_CONTAINER(frame), box);
    p->cal = gtk_calendar_new();
    GDate cur;
    if (parse(gtk_entry_get_text(e), &cur)) {
        gtk_calendar_select_month(GTK_CALENDAR(p->cal), g_date_get_month(&cur) - 1, g_date_get_year(&cur));
        gtk_calendar_select_day(GTK_CALENDAR(p->cal), g_date_get_day(&cur));
    }
    g_signal_connect(p->cal, "month-changed", G_CALLBACK(on_month_changed), p);
    g_signal_connect(p->cal, "day-selected", G_CALLBACK(on_day_selected), p);
    gtk_box_pack_start(GTK_BOX(box), p->cal, TRUE, TRUE, 0);
    GtkWidget *clear = gtk_button_new_from_stock(GTK_STOCK_CLEAR);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), p);
    GtkWidget *row = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    g_signal_connect(p->win, "button-press-event", G_CALLBACK(on_popup_button), p);
    g_signal_connect(p->win, "key-press-event", G_CALLBACK(on_popup_key), p);

    /* under the entry, kept on its monitor */
    int x, y;
    GtkAllocation a;
    gdk_window_get_origin(gtk_widget_get_window(p->entry), &x, &y);
    gtk_widget_get_allocation(p->entry, &a);
    GtkRequisition r;
    gtk_widget_show_all(frame);
    gtk_widget_size_request(p->win, &r);
    GdkScreen *scr = gtk_widget_get_screen(p->entry);
    GdkRectangle mon;
    gdk_screen_get_monitor_geometry(scr, gdk_screen_get_monitor_at_window(scr, gtk_widget_get_window(p->entry)), &mon);
    x = CLAMP(x + a.x + a.width - r.width, mon.x, mon.x + mon.width - r.width);
    y += a.y + a.height;
    if (y + r.height > mon.y + mon.height)
        y -= a.height + r.height;
    gtk_window_move(GTK_WINDOW(p->win), x, y);
    gtk_widget_show(p->win);

    guint32 t = ev ? gdk_event_get_time(ev) : GDK_CURRENT_TIME;
    GdkWindow *gw = gtk_widget_get_window(p->win);
    gtk_grab_add(p->win);
    if (gdk_pointer_grab(gw, TRUE, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK,
                         NULL, NULL, t) != GDK_GRAB_SUCCESS ||
        gdk_keyboard_grab(gw, TRUE, t) != GDK_GRAB_SUCCESS) {
        popup_close(p);
        return;
    }
    gtk_widget_grab_focus(p->cal);
}

GtkWidget *kfd_date_new(void)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(e), 11);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(e), GTK_ENTRY_ICON_SECONDARY, "x-office-calendar");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(e), GTK_ENTRY_ICON_SECONDARY, _("Pick a date"));
    g_signal_connect(e, "icon-press", G_CALLBACK(on_icon_press), NULL);
    return e;
}
