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
 * National holidays are a separate, built-in table (holidays_br(),
 * below) rather than something read from either file: they don't
 * change from one user/session to the next, so there's nothing to
 * edit. Only pt_BR fixed dates + the Easter-derived ones are covered
 * for now -- see holidays_br()'s own comment.
 */
#include "../xisserve.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static GtkWidget *g_calendar;
static GtkWidget *g_now_label;
static GtkWidget *g_zones_label;
static GtkWidget *g_events_label;
static guint g_tick_id;

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

/* ---- pt_BR national holidays -----------------------------------------
 * Fixed-date ones, plus the movable ones derived from Easter (Gauss'
 * algorithm, Gregorian calendar -- accurate for any year this widget
 * will ever show). Only Brazil is covered: there's no per-user locale
 * setting anywhere else in the project this could key off yet, and a
 * general multi-country table is a bigger thing to design (which
 * country codes, where the list itself comes from) than this page
 * needs today. */
static void easter_month_day(int year, int *month, int *day)
{
    int a = year % 19;
    int b = year / 100;
    int c = year % 100;
    int d = b / 4;
    int e = b % 4;
    int f = (b + 8) / 25;
    int g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4;
    int k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    int month_ = (h + l - 7 * m + 114) / 31;
    int day_ = ((h + l - 7 * m + 114) % 31) + 1;
    *month = month_;
    *day = day_;
}

/* Adds `offset` days (may be negative) to a Gregorian (year,month,day)
 * via time_t/struct tm's own normalization -- simplest correct way to
 * get from Easter to Carnaval (-47 days) / Corpus Christi (+60 days)
 * without hand-rolling calendar arithmetic. */
static void add_days(int year, int month, int day, int offset, int *out_month, int *out_day)
{
    struct tm tmv = {0};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day + offset;
    tmv.tm_hour = 12; /* noon: keeps DST transitions from shifting the date */
    time_t t = mktime(&tmv);
    struct tm norm;
    localtime_r(&t, &norm);
    *out_month = norm.tm_mon + 1;
    *out_day = norm.tm_mday;
}

/* Appends every pt_BR holiday name falling on (year,month,day) to buf,
 * one per line. */
static void holidays_br(int year, int month, int day, GString *buf)
{
    static const struct {
        int month, day;
        const char *name;
    } fixed[] = {
        {1, 1, "Confraterniza\xc3\xa7\xc3\xa3o Universal"},
        {4, 21, "Tiradentes"},
        {5, 1, "Dia do Trabalho"},
        {9, 7, "Independ\xc3\xaancia do Brasil"},
        {10, 12, "Nossa Senhora Aparecida"},
        {11, 2, "Finados"},
        {11, 15, "Proclama\xc3\xa7\xc3\xa3o da Rep\xc3\xba""blica"},
        {11, 20, "Consci\xc3\xaancia Negra"},
        {12, 25, "Natal"},
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (fixed[i].month == month && fixed[i].day == day) {
            g_string_append_printf(buf, "%s\n", fixed[i].name);
        }
    }

    int em, ed;
    easter_month_day(year, &em, &ed);
    if (em == month && ed == day) {
        g_string_append(buf, "P\xc3\xa1scoa\n");
    }
    int cm, cd;
    add_days(year, em, ed, -47, &cm, &cd);
    if (cm == month && cd == day) {
        g_string_append(buf, "Carnaval\n");
    }
    add_days(year, em, ed, -2, &cm, &cd);
    if (cm == month && cd == day) {
        g_string_append(buf, "Sexta-feira Santa\n");
    }
    add_days(year, em, ed, 60, &cm, &cd);
    if (cm == month && cd == day) {
        g_string_append(buf, "Corpus Christi\n");
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
    holidays_br((int)year, (int)month, (int)day, buf);
    load_events_for_day((int)year, (int)month, (int)day, buf);

    if (buf->len == 0) {
        gtk_label_set_text(GTK_LABEL(g_events_label), "Nenhum feriado ou evento neste dia.");
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

GtkWidget *page_calendar_build(void)
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

    GtkWidget *open_events_btn = gtk_button_new_with_label("Abrir eventos...");
    g_signal_connect(open_events_btn, "clicked", G_CALLBACK(on_open_events_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(left), open_events_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), left, TRUE, TRUE, 0);

    /* Right panel: the calendar itself. */
    g_calendar = gtk_calendar_new();
    g_signal_connect(g_calendar, "day-selected", G_CALLBACK(on_day_selected), NULL);
    g_signal_connect(g_calendar, "month-changed", G_CALLBACK(on_day_selected), NULL);
    gtk_box_pack_start(GTK_BOX(outer), g_calendar, FALSE, FALSE, 0);

    return outer;
}

void page_calendar_on_show(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    gtk_calendar_select_month(GTK_CALENDAR(g_calendar), (guint)tmv.tm_mon, (guint)(tmv.tm_year + 1900));
    gtk_calendar_select_day(GTK_CALENDAR(g_calendar), (guint)tmv.tm_mday);

    refresh_now_and_zones();
    refresh_events();
    if (!g_tick_id) {
        g_tick_id = g_timeout_add_seconds(1, on_tick, NULL);
    }
}

void page_calendar_on_hide(void)
{
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
}
