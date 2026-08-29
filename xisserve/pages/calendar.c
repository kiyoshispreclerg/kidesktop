/*
 * calendar.c - the --calendar page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section). A plain GtkCalendar: month/year navigation (the
 * header's prev/next arrows and a directly editable year) and bolding
 * whichever day is "today" are both built into the widget, so this page
 * is little more than "create it, and jump it back to the current month
 * every time it's shown".
 *
 * That jump matters: GTK only bolds today's date while the month
 * actually containing it is the one on screen, so a page left on some
 * other month from a previous open would come back with nothing
 * highlighted. Resetting on every show also matches the launcher view
 * always coming back on the Favoritos category rather than wherever it
 * was left.
 */
#include "../xisserve.h"

#include <time.h>

static GtkWidget *g_calendar;

GtkWidget *page_calendar_build(void)
{
    g_calendar = gtk_calendar_new();
    return g_calendar;
}

void page_calendar_on_show(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    gtk_calendar_select_month(GTK_CALENDAR(g_calendar), (guint)tmv.tm_mon, (guint)(tmv.tm_year + 1900));
    gtk_calendar_select_day(GTK_CALENDAR(g_calendar), (guint)tmv.tm_mday);
}
