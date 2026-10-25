/*
 * calendar.c - the --calendar page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section). Two panes: a GtkCalendar on the right (month/year
 * navigation and "today" bolding are built into the widget, so it's
 * just "create it, and jump it back to the current month every time
 * it's shown" -- see page_calendar_on_show()), and a text panel on the
 * left with the local date/time (big), a handful of other timezones
 * underneath, and the holidays/events falling on whichever day is
 * selected in the calendar.
 *
 * Two read-only config files, both in the same $XDG_CONFIG_HOME (or
 * ~/.config) directory xisserve.conf/xisserve-favorites.conf already
 * live in, both actually *edited* through kiconf rather than by hand:
 *
 *   ki-zones.conf -- "ZONE\t<IANA name>\t<label>" lines, one extra
 *     timezone clock per line (label empty = show the IANA name
 *     itself). Written by kiconf's Sistema tab.
 *   ki-events.conf -- "EVENT\t<YYYY-MM-DD>\t<recurrence>\t<name>\t<description>"
 *     lines, the user's own events/reminders (recurrence: NONE,
 *     YEARLY, MONTHLY, WEEKLY, DAILY). Written by kiconf's Eventos tab,
 *     which is also where "Abrir eventos" below sends the user --
 *     this page only ever reads the file.
 *
 * Since 0.1.57 the page is a notebook: this calendar tab, then the
 * Alarmes/Cronometro/Temporizador tabs, which live in clock.c.
 *
 * Public holidays are a third source: per-country data files read by
 * shared/xis_holidays.c (see its header), picked by the system timezone.
 * Nothing to edit from here; kiconf's Eventos tab has a "Ver feriados" list.
 */
#include "../xisserve.h"
#include "clock.h"
#include "../../shared/xis_holidays.h"

#include <langinfo.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static GtkWidget *g_calendar;
static GtkWidget *g_now_label;
static GtkWidget *g_zones_label;
static GtkWidget *g_events_label;
static guint g_tick_id;
static GtkWidget *g_notebook;

/* ---- ki-zones.conf / ki-events.conf: same "$XDG_CONFIG_HOME (or
 * ~/.config)/<name>" resolution xisserve.c's own config_path()/
 * favorites_path() use, so kiconf (which resolves paths the same way,
 * see kiconf/common.c's resolve_path()) and this page always agree on
 * where the file is. */
static void config_file_path(const char *name, char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/%s", xdg_config, name);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, outsz, "%s/.config/%s", home ? home : "/tmp", name);
}

/* ---- other timezones -----------------------------------------------
 * localtime_in_tz()/ZoneEntry mirror xispanel/widgets/clock.c's own
 * tz=/tooltip_tz= handling exactly (same TZ-env-var trick, same
 * reasoning in the comment there) -- kept as a separate copy since
 * xisserve and xispanel are different daemons/processes with no shared
 * runtime to call into. */
#define MAX_ZONES 16
typedef struct {
    char iana[64];
    char label[64];
} ZoneEntry;

static void localtime_in_tz(const char *tz, struct tm *out)
{
    char old[128];
    int had_old = 0;
    const char *cur = getenv("TZ");
    if (cur) {
        snprintf(old, sizeof(old), "%s", cur);
        had_old = 1;
    }
    if (tz && tz[0]) {
        setenv("TZ", tz, 1);
        tzset();
    }
    time_t t = time(NULL);
    localtime_r(&t, out);
    if (tz && tz[0]) {
        if (had_old) {
            setenv("TZ", old, 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }
}

static int load_zones(ZoneEntry *out, int max)
{
    char path[4096];
    config_file_path("ki-zones.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int n = 0;
    char line[512];
    while (n < max && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *save = NULL;
        char *tag = strtok_r(line, "\t", &save);
        char *iana = tag ? strtok_r(NULL, "\t", &save) : NULL;
        if (!tag || strcmp(tag, "ZONE") != 0 || !iana || !iana[0]) {
            continue;
        }
        char *label = strtok_r(NULL, "\t", &save);
        snprintf(out[n].iana, sizeof(out[n].iana), "%s", iana);
        snprintf(out[n].label, sizeof(out[n].label), "%s", (label && label[0]) ? label : iana);
        n++;
    }
    fclose(f);
    return n;
}

/* ---- public holidays ---------------------------------------------------
 * From shared/xis_holidays.c: the data file of the country the system
 * timezone belongs to. The month's list is looked up per call; the library
 * caches the parsed file (and re-reads it when it changes on disk). */
static void holidays_for_day(int year, int month, int day, GString *buf)
{
    static char country[8];
    static time_t country_at;
    time_t now = time(NULL);
    if (!country_at || now - country_at > 60) { /* the timezone can change while we run */
        xis_holidays_country(country, sizeof(country));
        country_at = now;
    }
    XisHoliday list[64];
    int n = xis_holidays_for_year(country, year, list, 64);
    for (int i = 0; i < n; i++) {
        if (list[i].month == month && list[i].day == day) {
            g_string_append_printf(buf, "%s\n", list[i].name);
        }
    }
}

/* ---- ki-events.conf --------------------------------------------------- */

typedef enum { RECUR_NONE, RECUR_YEARLY, RECUR_MONTHLY, RECUR_WEEKLY, RECUR_DAILY } Recurrence;

static Recurrence parse_recurrence(const char *s)
{
    if (!strcmp(s, "YEARLY")) return RECUR_YEARLY;
    if (!strcmp(s, "MONTHLY")) return RECUR_MONTHLY;
    if (!strcmp(s, "WEEKLY")) return RECUR_WEEKLY;
    if (!strcmp(s, "DAILY")) return RECUR_DAILY;
    return RECUR_NONE;
}

/* True iff an event anchored at (ay,am,ad) with recurrence `r` occurs
 * on (y,m,d) -- (y,m,d) must not be before the anchor date itself. */
static int event_occurs(Recurrence r, int ay, int am, int ad, int y, int m, int d)
{
    struct tm a = {0}, b = {0};
    a.tm_year = ay - 1900; a.tm_mon = am - 1; a.tm_mday = ad; a.tm_hour = 12;
    b.tm_year = y - 1900; b.tm_mon = m - 1; b.tm_mday = d; b.tm_hour = 12;
    time_t ta = mktime(&a), tb = mktime(&b);
    if (tb < ta) {
        return 0;
    }
    switch (r) {
    case RECUR_NONE:
        return ay == y && am == m && ad == d;
    case RECUR_YEARLY:
        return am == m && ad == d;
    case RECUR_MONTHLY:
        return ad == d;
    case RECUR_WEEKLY: {
        long days = (tb - ta) / 86400;
        return days % 7 == 0;
    }
    case RECUR_DAILY:
        return 1;
    }
    return 0;
}

/* Appends every ki-events.conf entry occurring on (year,month,day) to
 * buf, as "name -- description" (or just "name" if there's no
 * description), one per line. */
static void load_events_for_day(int year, int month, int day, GString *buf)
{
    char path[4096];
    config_file_path("ki-events.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *save = NULL;
        char *tag = strtok_r(line, "\t", &save);
        char *date = tag ? strtok_r(NULL, "\t", &save) : NULL;
        char *recur = date ? strtok_r(NULL, "\t", &save) : NULL;
        char *name = recur ? strtok_r(NULL, "\t", &save) : NULL;
        char *desc = name ? strtok_r(NULL, "", &save) : NULL;
        if (!tag || strcmp(tag, "EVENT") != 0 || !date || !recur || !name || !name[0]) {
            continue;
        }
        int ay, am, ad;
        if (sscanf(date, "%d-%d-%d", &ay, &am, &ad) != 3) {
            continue;
        }
        if (!event_occurs(parse_recurrence(recur), ay, am, ad, year, month, day)) {
            continue;
        }
        if (desc && desc[0]) {
            g_string_append_printf(buf, "%s \xe2\x80\x94 %s\n", name, desc);
        } else {
            g_string_append_printf(buf, "%s\n", name);
        }
    }
    fclose(f);
}

/* ---- event marks under the day numbers ----------------------------------
 *
 * GtkCalendar's own detail rows draw a separator line between the number
 * and the detail, so the marks are painted by hand instead: an expose
 * handler (after GTK's) draws a short bar at the bottom of each marked
 * day's cell. The day grid is the calendar's biggest child GdkWindow,
 * split 7x6; a day's cell comes from the weekday of the 1st and the
 * locale's week start (the same rule GtkCalendar applies). One month's
 * worth of marks is cached as a bitmask since the handler runs on every
 * redraw. Colour is the theme's text colour for now; once events carry a
 * type, this is where each type's colour goes. Days of the neighbouring
 * months shown greyed out in the grid are not marked. */

static int g_marks_year, g_marks_month; /* 1-based month; 0 = nothing cached */
static guint32 g_marks;

static void compute_marks(int year, int month)
{
    g_marks_year = year;
    g_marks_month = month;
    g_marks = 0;
    for (int d = 1; d <= 31; d++) {
        GString *buf = g_string_new(NULL);
        holidays_for_day(year, month, d, buf);
        load_events_for_day(year, month, d, buf);
        if (buf->len) g_marks |= 1u << (d - 1);
        g_string_free(buf, TRUE);
    }
}

/* Column (0 = Sunday-based weekday) the calendar puts first. */
static int calendar_week_start(void)
{
    int first = nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0];
    int origin_date = (int)(intptr_t)nl_langinfo(_NL_TIME_WEEK_1STDAY);
    int origin = origin_date == 19971201 ? 1 : origin_date == 19971202 ? 2 : 0;
    return (origin + first - 1) % 7;
}

static gboolean on_calendar_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)data;
    GdkWindow *grid = NULL;
    int grid_h = 0;
    GList *children = gdk_window_get_children(w->window);
    for (GList *l = children; l; l = l->next) {
        int h;
        gdk_window_get_geometry(l->data, NULL, NULL, NULL, &h, NULL);
        if (h > grid_h) {
            grid_h = h;
            grid = l->data;
        }
    }
    g_list_free(children);
    if (!grid || ev->window != grid) return FALSE;

    guint year, month, sel;
    gtk_calendar_get_date(GTK_CALENDAR(w), &year, &month, &sel);
    if (g_marks_year != (int)year || g_marks_month != (int)month + 1) compute_marks((int)year, (int)month + 1);
    if (!g_marks) return FALSE;

    int grid_w;
    gdk_window_get_geometry(grid, NULL, NULL, &grid_w, NULL, NULL);
    struct tm first = {0};
    first.tm_year = (int)year - 1900;
    first.tm_mon = (int)month;
    first.tm_mday = 1;
    mktime(&first);
    int col0 = (first.tm_wday - calendar_week_start() + 7) % 7;
    double cw = (double)grid_w / 7, ch = (double)grid_h / 6;

    cairo_t *cr = gdk_cairo_create(grid);
    for (int d = 1; d <= 31; d++) {
        int idx = col0 + d - 1;
        if (idx >= 42) break;
        if (!(g_marks & (1u << (d - 1)))) continue;
        gdk_cairo_set_source_color(cr, &w->style->fg[d == (int)sel ? GTK_STATE_SELECTED : GTK_STATE_NORMAL]);
        cairo_rectangle(cr, (idx % 7) * cw + cw / 2 - 5, (idx / 7) * ch + ch - 4, 10, 2);
        cairo_fill(cr);
    }
    cairo_destroy(cr);
    return FALSE;
}

/* ---- left panel refresh ------------------------------------------------ */

static void refresh_now_and_zones(void)
{
    struct tm now;
    localtime_in_tz(NULL, &now);
    char buf[256];
    strftime(buf, sizeof(buf), "%A, %d de %B", &now);
    buf[0] = g_ascii_toupper(buf[0]);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M", &now);
    char markup[512];
    snprintf(markup, sizeof(markup),
             "<span size=\"xx-large\" weight=\"bold\">%s</span>\n<span size=\"large\">%s</span>",
             timebuf, buf);
    gtk_label_set_markup(GTK_LABEL(g_now_label), markup);

    ZoneEntry zones[MAX_ZONES];
    int n = load_zones(zones, MAX_ZONES);
    if (n == 0) {
        gtk_widget_hide(g_zones_label);
    } else {
        GString *zbuf = g_string_new(NULL);
        for (int i = 0; i < n; i++) {
            struct tm zt;
            localtime_in_tz(zones[i].iana, &zt);
            char zline[128];
            strftime(zline, sizeof(zline), "%H:%M", &zt);
            g_string_append_printf(zbuf, "%s: %s\n", zones[i].label, zline);
        }
        gtk_label_set_text(GTK_LABEL(g_zones_label), zbuf->str);
        g_string_free(zbuf, TRUE);
        gtk_widget_show(g_zones_label);
    }
}

static void refresh_events(void)
{
    guint year, month, day;
    gtk_calendar_get_date(GTK_CALENDAR(g_calendar), &year, &month, &day);
    month += 1; /* GtkCalendar's month is 0-based */

    GString *buf = g_string_new(NULL);
    holidays_for_day((int)year, (int)month, (int)day, buf);
    load_events_for_day((int)year, (int)month, (int)day, buf);

    if (buf->len == 0) {
        gtk_label_set_text(GTK_LABEL(g_events_label), _("Nenhum feriado ou evento neste dia."));
    } else {
        gtk_label_set_text(GTK_LABEL(g_events_label), buf->str);
    }
    g_string_free(buf, TRUE);
}

static gboolean on_tick(gpointer data)
{
    (void)data;
    refresh_now_and_zones();
    return TRUE;
}

static void on_day_selected(GtkCalendar *cal, gpointer data)
{
    (void)cal;
    (void)data;
    refresh_events();
}

static void on_open_events_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    run_detached("kiconf --tab Eventos");
}

/* Notebook page index -> which clock tab that is (0 is the calendar). */
static ClockTab clock_tab_for_page(int page)
{
    static const ClockTab tabs[] = {CLOCK_TAB_NONE, CLOCK_TAB_ALARMS, CLOCK_TAB_STOPWATCH, CLOCK_TAB_TIMERS};
    return page >= 0 && page < (int)G_N_ELEMENTS(tabs) ? tabs[page] : CLOCK_TAB_NONE;
}

static void on_switch_page(GtkNotebook *nb, gpointer page, guint num, gpointer data)
{
    (void)nb;
    (void)page;
    (void)data;
    clock_set_visible_tab(clock_tab_for_page((int)num));
}

static GtkWidget *build_calendar_tab(void)
{
    GtkWidget *outer = gtk_hbox_new(FALSE, 12);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    /* Left panel: local date/time, other zones, selected day's events. */
    GtkWidget *left = gtk_vbox_new(FALSE, 8);
    gtk_widget_set_size_request(left, 220, -1);

    g_now_label = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(g_now_label), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_now_label), TRUE);
    gtk_box_pack_start(GTK_BOX(left), g_now_label, FALSE, FALSE, 0);

    g_zones_label = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(g_zones_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(left), g_zones_label, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(left), sep, FALSE, FALSE, 4);

    GtkWidget *events_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(events_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_events_label = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(g_events_label), 0.0, 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(g_events_label), TRUE);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(events_scroll), g_events_label);
    gtk_box_pack_start(GTK_BOX(left), events_scroll, TRUE, TRUE, 0);

    GtkWidget *open_events_btn = gtk_button_new_with_label(_("Abrir eventos..."));
    g_signal_connect(open_events_btn, "clicked", G_CALLBACK(on_open_events_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(left), open_events_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), left, TRUE, TRUE, 0);

    /* Right panel: the calendar itself. */
    /* The day cells are sized from the focus padding/line width, which
     * themes set to anything (Numix: padding 0, so the rows come out ~2px
     * tighter than with the default theme). Pin them to GTK's defaults so
     * the spacing -- and the marks drawn into it -- looks the same
     * everywhere. */
    gtk_rc_parse_string("style \"xisserve-calendar\" {\n"
                        "  GtkWidget::focus-padding = 1\n"
                        "  GtkWidget::focus-line-width = 1\n"
                        "}\n"
                        "widget \"*xisserve-calendar\" style \"xisserve-calendar\"");
    g_calendar = gtk_calendar_new();
    gtk_widget_set_name(g_calendar, "xisserve-calendar");
    xisserve_a11y(g_calendar, _("Calend\xc3\xa1rio"), NULL);
    g_signal_connect_after(g_calendar, "expose-event", G_CALLBACK(on_calendar_expose), NULL);
    /* GtkCalendar has no row-spacing property (vertical-separation only
     * spaces the day names), so its row height follows the font: a bit
     * larger text gives the rows -- and the marks -- room to breathe. */
    PangoFontDescription *fd = pango_font_description_copy(gtk_widget_get_style(g_calendar)->font_desc);
    pango_font_description_set_size(fd, (int)(pango_font_description_get_size(fd) * 1.15));
    gtk_widget_modify_font(g_calendar, fd);
    pango_font_description_free(fd);
    g_signal_connect(g_calendar, "day-selected", G_CALLBACK(on_day_selected), NULL);
    g_signal_connect(g_calendar, "month-changed", G_CALLBACK(on_day_selected), NULL);
    gtk_box_pack_start(GTK_BOX(outer), g_calendar, FALSE, FALSE, 0);

    return outer;
}

/* The calendar (whose left panel is also the world clock) plus the
 * Android-clock-style tabs from clock.c. */
GtkWidget *page_calendar_build(void)
{
    g_notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), build_calendar_tab(), gtk_label_new(_("Calend\xc3\xa1rio")));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), clock_alarms_build(), gtk_label_new(_("Alarmes")));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), clock_stopwatch_build(), gtk_label_new(_("Cron\xc3\xb4metro")));
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), clock_timers_build(), gtk_label_new(_("Temporizador")));
    g_signal_connect(g_notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);
    return g_notebook;
}

void page_calendar_on_show(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    gtk_calendar_select_month(GTK_CALENDAR(g_calendar), (guint)tmv.tm_mon, (guint)(tmv.tm_year + 1900));
    gtk_calendar_select_day(GTK_CALENDAR(g_calendar), (guint)tmv.tm_mday);

    g_marks_month = 0; /* events may have changed while hidden */
    gtk_widget_queue_draw(g_calendar);
    refresh_now_and_zones();
    refresh_events();
    if (!g_tick_id) {
        g_tick_id = g_timeout_add_seconds(1, on_tick, NULL);
    }
    clock_on_show();
    clock_set_visible_tab(clock_tab_for_page(gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook))));
}

void page_calendar_on_hide(void)
{
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
    clock_on_hide();
}
