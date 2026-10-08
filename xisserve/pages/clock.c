/*
 * clock.c - the --calendar page's Alarmes / Cronometro / Temporizador
 * tabs (calendar.c packs them into its notebook next to the calendar
 * itself, whose left panel already is the "world clock").
 *
 * All state lives in ki-clock.conf (../../shared/xis_clock.h), never in
 * this process: a running stopwatch or timer is just a start/end
 * timestamp, so it keeps counting with the page closed or xisserve gone,
 * and kiconfd -- not this page -- rings whatever comes due (through
 * `xisserve --ring`, ring.c). Every change here is load, change, save in
 * one go, and the display reloads whenever the file's mtime moves, so
 * edits from the ring popup ("Soneca", "+1 min") show up live.
 *
 * The display only ticks while one of these tabs is visible, and only
 * fast (for the stopwatch's hundredths) while something is running.
 */
#include "../xisserve.h"
#include "../../shared/xis_clock.h"
#include "clock.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static XisClock g_clk;
static struct timespec g_clk_mtime;
static ClockTab g_tab = CLOCK_TAB_NONE;
static guint g_tick_id;
static guint g_tick_ms;

/* Alarms tab */
static GtkWidget *g_alarm_list;     /* vbox of rows, rebuilt on change */
static GtkWidget *g_alarm_hour, *g_alarm_min, *g_alarm_label;
static GtkWidget *g_alarm_days[7];
static GtkWidget *g_alarm_add_btn, *g_alarm_cancel_btn;
static int g_alarm_editing; /* id being edited, 0 = adding a new one */
static GtkWidget *g_alarm_warning;

/* Stopwatch tab */
static GtkWidget *g_sw_label;
static GtkWidget *g_sw_start_btn, *g_sw_lap_btn, *g_sw_reset_btn;
static GtkListStore *g_sw_laps;
static int g_sw_shown_laps = -1;

/* Timers tab */
static GtkWidget *g_timer_list;
static GtkWidget *g_timer_h, *g_timer_m, *g_timer_s, *g_timer_label;
static GtkWidget *g_timer_warning;

static void refresh_all(void);
static void reschedule_tick(void);

/* ---- state ---------------------------------------------------------------- */

static void remember_mtime(void)
{
    char path[PATH_MAX];
    xis_clock_path(path, sizeof(path));
    struct stat st;
    if (stat(path, &st) == 0) g_clk_mtime = st.st_mtim;
    else memset(&g_clk_mtime, 0, sizeof(g_clk_mtime));
}

static gboolean reload_if_changed(void)
{
    char path[PATH_MAX];
    xis_clock_path(path, sizeof(path));
    struct stat st;
    struct timespec m = {0, 0};
    if (stat(path, &st) == 0) m = st.st_mtim;
    if (m.tv_sec == g_clk_mtime.tv_sec && m.tv_nsec == g_clk_mtime.tv_nsec) return FALSE;
    xis_clock_load(&g_clk);
    g_clk_mtime = m;
    return TRUE;
}

/* Every edit: fresh load (someone else may have written since), change,
 * save. Usage: clock_begin(); ...change g_clk...; clock_commit(); */
static void clock_begin(void)
{
    xis_clock_load(&g_clk);
}

static void clock_commit(void)
{
    if (xis_clock_save(&g_clk) != 0) perror("xisserve: save ki-clock.conf");
    remember_mtime();
    refresh_all();
    reschedule_tick();
}

/* Without kiconfd nothing rings -- worth saying where alarms are set. */
static gboolean kiconfd_running(void)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/kiconfd.lock", (rundir && *rundir) ? rundir : "/tmp");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    gboolean held = flock(fd, LOCK_SH | LOCK_NB) != 0;
    close(fd); /* also drops our probe lock if we got it */
    return held;
}

/* ---- formatting ----------------------------------------------------------- */

static void format_hms(long long ms, gboolean round_up, char *out, size_t outsz)
{
    long long s = round_up ? (ms + 999) / 1000 : ms / 1000;
    if (s >= 3600) snprintf(out, outsz, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, outsz, "%02lld:%02lld", s / 60, s % 60);
}

static void format_stopwatch(long long ms, char *out, size_t outsz)
{
    char hms[32];
    format_hms(ms, FALSE, hms, sizeof(hms));
    snprintf(out, outsz, "%s.%02lld", hms, (ms / 10) % 100);
}

static const char *const kDayShort[7] = {N_("Dom"), N_("Seg"), N_("Ter"), N_("Qua"), N_("Qui"), N_("Sex"), N_("S\xc3\xa1" "b")};
static const char *const kDayLetter[7] = {N_("D"), N_("S"), N_("T"), N_("Q"), N_("Q"), N_("S"), N_("S")};
static const char *const kDayLong[7] = {N_("Domingo"), N_("Segunda-feira"), N_("Ter\xc3\xa7" "a-feira"), N_("Quarta-feira"),
                                        N_("Quinta-feira"), N_("Sexta-feira"), N_("S\xc3\xa1" "bado")};

static void format_days(unsigned days, char *out, size_t outsz)
{
    if (days == 0) snprintf(out, outsz, "%s", _("Uma vez"));
    else if (days == 0x7f) snprintf(out, outsz, "%s", _("Todos os dias"));
    else if (days == 0x3e) snprintf(out, outsz, "%s", _("Dias \xc3\xbateis"));
    else if (days == 0x41) snprintf(out, outsz, "%s", _("Fins de semana"));
    else {
        out[0] = 0;
        for (int d = 0; d < 7; d++) {
            if (!(days & (1u << d))) continue;
            size_t l = strlen(out);
            snprintf(out + l, outsz - l, "%s%s", l ? ", " : "", _(kDayShort[d]));
        }
    }
}

/* "em 7 h 12 min" -- how far away the next ring is. */
static void format_until(long long secs, char *out, size_t outsz)
{
    long long m = (secs + 59) / 60;
    if (m < 60) snprintf(out, outsz, _("em %lld min"), m);
    else if (m < 24 * 60) snprintf(out, outsz, _("em %lld h %lld min"), m / 60, m % 60);
    else snprintf(out, outsz, _("em %lld d %lld h"), m / (24 * 60), (m / 60) % 24);
}

/* ---- alarms tab ----------------------------------------------------------- */

static void alarm_row_markup(const XisAlarm *a, long long now, GtkWidget *label)
{
    char days[96], when[64] = "";
    format_days(a->days, days, sizeof(days));
    long long next = xis_alarm_next(a, now);
    if (next) format_until(next - now, when, sizeof(when));
    gchar *name = g_markup_escape_text(a->label, -1);
    gchar *markup;
    if (a->snooze_until > now && a->enabled) {
        char snooze[32];
        struct tm st;
        time_t su = (time_t)a->snooze_until;
        localtime_r(&su, &st);
        strftime(snooze, sizeof(snooze), "%H:%M", &st);
        markup = g_strdup_printf("<span size=\"x-large\" weight=\"bold\">%02d:%02d</span>  %s\n<small>%s \xc2\xb7 %s %s</small>",
                                 a->hour, a->minute, name, days, _("soneca at\xc3\xa9"), snooze);
    } else {
        markup = g_strdup_printf("<span size=\"x-large\" weight=\"bold\"%s>%02d:%02d</span>  %s\n<small>%s%s%s</small>",
                                 next ? "" : " foreground=\"#888888\"", a->hour, a->minute, name, days,
                                 when[0] ? " \xc2\xb7 " : "", when);
    }
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(name);
}

static void on_alarm_toggled(GtkToggleButton *btn, gpointer data)
{
    int id = GPOINTER_TO_INT(data);
    clock_begin();
    XisAlarm *a = xis_clock_find_alarm(&g_clk, id);
    if (!a) return;
    a->enabled = gtk_toggle_button_get_active(btn);
    a->snooze_until = 0;
    if (a->enabled && !a->days) a->once_at = xis_alarm_next_occurrence(a->hour, a->minute, 0, (long long)time(NULL));
    clock_commit();
}

static void alarm_editor_reset(void)
{
    g_alarm_editing = 0;
    gtk_entry_set_text(GTK_ENTRY(g_alarm_label), "");
    for (int d = 0; d < 7; d++) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alarm_days[d]), FALSE);
    gtk_button_set_label(GTK_BUTTON(g_alarm_add_btn), _("Adicionar alarme"));
    gtk_widget_hide(g_alarm_cancel_btn);
}

static void on_alarm_edit(GtkWidget *btn, gpointer data)
{
    (void)btn;
    XisAlarm *a = xis_clock_find_alarm(&g_clk, GPOINTER_TO_INT(data));
    if (!a) return;
    g_alarm_editing = a->id;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_alarm_hour), a->hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_alarm_min), a->minute);
    gtk_entry_set_text(GTK_ENTRY(g_alarm_label), a->label);
    for (int d = 0; d < 7; d++) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alarm_days[d]), (a->days >> d) & 1);
    gtk_button_set_label(GTK_BUTTON(g_alarm_add_btn), _("Salvar alarme"));
    gtk_widget_show(g_alarm_cancel_btn);
}

static void on_alarm_delete(GtkWidget *btn, gpointer data)
{
    (void)btn;
    int id = GPOINTER_TO_INT(data);
    clock_begin();
    xis_clock_remove_alarm(&g_clk, id);
    if (g_alarm_editing == id) alarm_editor_reset();
    clock_commit();
}

static void on_alarm_add(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    clock_begin();
    XisAlarm *a = g_alarm_editing ? xis_clock_find_alarm(&g_clk, g_alarm_editing) : NULL;
    if (!a) a = xis_clock_add_alarm(&g_clk);
    if (!a) return; /* full */
    a->hour = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_alarm_hour));
    a->minute = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_alarm_min));
    a->days = 0;
    for (int d = 0; d < 7; d++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_alarm_days[d]))) a->days |= 1u << d;
    }
    a->once_at = a->days ? 0 : xis_alarm_next_occurrence(a->hour, a->minute, 0, (long long)time(NULL));
    a->enabled = 1;
    a->snooze_until = 0;
    snprintf(a->label, sizeof(a->label), "%s", gtk_entry_get_text(GTK_ENTRY(g_alarm_label)));
    alarm_editor_reset();
    clock_commit();
}

static void on_alarm_cancel(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    alarm_editor_reset();
}

static void clear_container(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static void rebuild_alarm_rows(void)
{
    clear_container(g_alarm_list);
    long long now = (long long)time(NULL);
    if (g_clk.nalarms == 0) {
        GtkWidget *empty = gtk_label_new(_("Nenhum alarme."));
        gtk_box_pack_start(GTK_BOX(g_alarm_list), empty, FALSE, FALSE, 8);
    }
    for (int i = 0; i < g_clk.nalarms; i++) {
        XisAlarm *a = &g_clk.alarms[i];
        GtkWidget *row = gtk_hbox_new(FALSE, 6);
        GtkWidget *check = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), a->enabled && (a->days || a->once_at > now || a->snooze_until > now));
        xisserve_a11y(check, _("Ativado"), NULL);
        g_signal_connect(check, "toggled", G_CALLBACK(on_alarm_toggled), GINT_TO_POINTER(a->id));
        gtk_box_pack_start(GTK_BOX(row), check, FALSE, FALSE, 0);

        GtkWidget *label = gtk_label_new(NULL);
        gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        alarm_row_markup(a, now, label);
        g_object_set_data(G_OBJECT(row), "label", label);
        g_object_set_data(G_OBJECT(row), "id", GINT_TO_POINTER(a->id));
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);

        GtkWidget *edit = gtk_button_new_with_label(_("Editar"));
        g_signal_connect(edit, "clicked", G_CALLBACK(on_alarm_edit), GINT_TO_POINTER(a->id));
        gtk_box_pack_start(GTK_BOX(row), edit, FALSE, FALSE, 0);
        GtkWidget *del = gtk_button_new_with_label("\xc3\x97");
        xisserve_a11y(del, _("Excluir alarme"), NULL);
        gtk_widget_set_tooltip_text(del, _("Excluir alarme"));
        g_signal_connect(del, "clicked", G_CALLBACK(on_alarm_delete), GINT_TO_POINTER(a->id));
        gtk_box_pack_start(GTK_BOX(row), del, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(g_alarm_list), row, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(g_alarm_list);
}

static void update_alarm_rows(void)
{
    long long now = (long long)time(NULL);
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_alarm_list));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *label = g_object_get_data(G_OBJECT(l->data), "label");
        XisAlarm *a = xis_clock_find_alarm(&g_clk, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "id")));
        if (label && a) alarm_row_markup(a, now, label);
    }
    g_list_free(children);
}

/* Two-digit minutes in the spin button ("07", not "7"). */
static gboolean on_spin_output_2digits(GtkSpinButton *spin, gpointer data)
{
    (void)data;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", gtk_spin_button_get_value_as_int(spin));
    if (strcmp(buf, gtk_entry_get_text(GTK_ENTRY(spin)))) gtk_entry_set_text(GTK_ENTRY(spin), buf);
    return TRUE;
}

static GtkWidget *time_spin(int max, const char *a11y)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(0, max, 1);
    gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    g_signal_connect(spin, "output", G_CALLBACK(on_spin_output_2digits), NULL);
    xisserve_a11y(spin, a11y, NULL);
    return spin;
}

static GtkWidget *warning_label(void)
{
    GtkWidget *w = gtk_label_new(_("O kiconfd n\xc3\xa3o est\xc3\xa1 em execu\xc3\xa7\xc3\xa3o: nada vai tocar."));
    gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
    gtk_misc_set_alignment(GTK_MISC(w), 0.0, 0.5);
    return w;
}

GtkWidget *clock_alarms_build(void)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);

    g_alarm_warning = warning_label();
    gtk_widget_set_no_show_all(g_alarm_warning, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), g_alarm_warning, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_alarm_list = gtk_vbox_new(FALSE, 4);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), g_alarm_list);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    /* Editor: HH : MM  label */
    GtkWidget *line1 = gtk_hbox_new(FALSE, 4);
    g_alarm_hour = time_spin(23, _("Hora"));
    g_alarm_min = time_spin(59, _("Minuto"));
    gtk_box_pack_start(GTK_BOX(line1), g_alarm_hour, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line1), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line1), g_alarm_min, FALSE, FALSE, 0);
    g_alarm_label = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(g_alarm_label), XIS_CLOCK_LABEL_LEN - 1);
    xisserve_a11y(g_alarm_label, _("Nome do alarme"), NULL);
    gtk_widget_set_tooltip_text(g_alarm_label, _("Nome do alarme (opcional)"));
    gtk_box_pack_start(GTK_BOX(line1), g_alarm_label, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), line1, FALSE, FALSE, 0);

    /* Repeat days; none = rings once. */
    GtkWidget *line2 = gtk_hbox_new(TRUE, 2);
    for (int d = 0; d < 7; d++) {
        g_alarm_days[d] = gtk_toggle_button_new_with_label(_(kDayLetter[d]));
        gtk_widget_set_tooltip_text(g_alarm_days[d], _(kDayLong[d]));
        xisserve_a11y(g_alarm_days[d], _(kDayLong[d]), NULL);
        gtk_box_pack_start(GTK_BOX(line2), g_alarm_days[d], TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), line2, FALSE, FALSE, 0);

    GtkWidget *line3 = gtk_hbox_new(FALSE, 4);
    g_alarm_add_btn = gtk_button_new_with_label(_("Adicionar alarme"));
    g_signal_connect(g_alarm_add_btn, "clicked", G_CALLBACK(on_alarm_add), NULL);
    g_alarm_cancel_btn = gtk_button_new_with_label(_("Cancelar"));
    g_signal_connect(g_alarm_cancel_btn, "clicked", G_CALLBACK(on_alarm_cancel), NULL);
    gtk_widget_set_no_show_all(g_alarm_cancel_btn, TRUE);
    gtk_box_pack_end(GTK_BOX(line3), g_alarm_add_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(line3), g_alarm_cancel_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), line3, FALSE, FALSE, 0);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_alarm_hour), 7);
    return vbox;
}

/* ---- stopwatch tab -------------------------------------------------------- */

static void update_stopwatch(void)
{
    long long now = xis_clock_now_ms();
    long long el = xis_stopwatch_elapsed(&g_clk.sw, now);
    char buf[48];
    format_stopwatch(el, buf, sizeof(buf));
    char markup[128];
    snprintf(markup, sizeof(markup), "<span size=\"xx-large\" weight=\"bold\" font_family=\"monospace\">%s</span>", buf);
    gtk_label_set_markup(GTK_LABEL(g_sw_label), markup);

    gboolean running = g_clk.sw.start_ms > 0;
    gtk_button_set_label(GTK_BUTTON(g_sw_start_btn), running ? _("Pausar") : el > 0 ? _("Continuar") : _("Iniciar"));
    gtk_widget_set_sensitive(g_sw_lap_btn, running && g_clk.sw.nlaps < XIS_CLOCK_MAX_LAPS);
    gtk_widget_set_sensitive(g_sw_reset_btn, el > 0);

    if (g_sw_shown_laps != g_clk.sw.nlaps) {
        g_sw_shown_laps = g_clk.sw.nlaps;
        gtk_list_store_clear(g_sw_laps);
        for (int i = g_clk.sw.nlaps - 1; i >= 0; i--) {
            char lap[48], total[48], num[12];
            format_stopwatch(g_clk.sw.laps[i] - (i > 0 ? g_clk.sw.laps[i - 1] : 0), lap, sizeof(lap));
            format_stopwatch(g_clk.sw.laps[i], total, sizeof(total));
            snprintf(num, sizeof(num), "%d", i + 1);
            GtkTreeIter it;
            gtk_list_store_append(g_sw_laps, &it);
            gtk_list_store_set(g_sw_laps, &it, 0, num, 1, lap, 2, total, -1);
        }
    }
}

static void on_sw_start(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    clock_begin();
    long long now = xis_clock_now_ms();
    if (g_clk.sw.start_ms > 0) {
        g_clk.sw.accum_ms = xis_stopwatch_elapsed(&g_clk.sw, now);
        g_clk.sw.start_ms = 0;
    } else {
        g_clk.sw.start_ms = now;
    }
    clock_commit();
}

static void on_sw_lap(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    clock_begin();
    if (g_clk.sw.start_ms > 0 && g_clk.sw.nlaps < XIS_CLOCK_MAX_LAPS) {
        g_clk.sw.laps[g_clk.sw.nlaps++] = xis_stopwatch_elapsed(&g_clk.sw, xis_clock_now_ms());
    }
    clock_commit();
}

static void on_sw_reset(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    clock_begin();
    memset(&g_clk.sw, 0, sizeof(g_clk.sw));
    clock_commit();
}

GtkWidget *clock_stopwatch_build(void)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);

    g_sw_label = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_sw_label, FALSE, FALSE, 8);

    GtkWidget *buttons = gtk_hbox_new(TRUE, 6);
    g_sw_reset_btn = gtk_button_new_with_label(_("Zerar"));
    g_signal_connect(g_sw_reset_btn, "clicked", G_CALLBACK(on_sw_reset), NULL);
    g_sw_lap_btn = gtk_button_new_with_label(_("Volta"));
    g_signal_connect(g_sw_lap_btn, "clicked", G_CALLBACK(on_sw_lap), NULL);
    g_sw_start_btn = gtk_button_new_with_label(_("Iniciar"));
    g_signal_connect(g_sw_start_btn, "clicked", G_CALLBACK(on_sw_start), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), g_sw_reset_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), g_sw_lap_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), g_sw_start_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), buttons, FALSE, FALSE, 0);

    g_sw_laps = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_sw_laps));
    g_object_unref(g_sw_laps); /* the view holds it now */
    static const char *const titles[3] = {N_("Volta"), N_("Tempo"), N_("Total")};
    for (int i = 0; i < 3; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i > 0) g_object_set(r, "family", "monospace", NULL);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(_(titles[i]), r, "text", i, NULL);
        gtk_tree_view_column_set_expand(col, i > 0);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    xisserve_a11y(tv, _("Voltas"), NULL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tv);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    return vbox;
}

/* ---- timers tab ----------------------------------------------------------- */

static void timer_row_update(const XisTimer *t, long long now, GtkWidget *row)
{
    GtkWidget *label = g_object_get_data(G_OBJECT(row), "label");
    GtkWidget *start = g_object_get_data(G_OBJECT(row), "start");
    long long rem = xis_timer_remaining(t, now);
    gboolean running = t->end_ms > 0 && rem > 0;
    gboolean done = t->end_ms > 0 && rem == 0;
    char big[32], dur[32];
    if (done) snprintf(big, sizeof(big), "%s", _("Terminado"));
    else format_hms(rem, TRUE, big, sizeof(big));
    format_hms(t->duration_ms, TRUE, dur, sizeof(dur));
    gchar *name = g_markup_escape_text(t->label, -1);
    gchar *markup = g_strdup_printf("<span size=\"x-large\" weight=\"bold\" font_family=\"monospace\">%s</span>\n<small>%s%s%s</small>",
                                    big, name, name[0] ? " \xc2\xb7 " : "", dur);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    g_free(name);
    gtk_button_set_label(GTK_BUTTON(start), running ? _("Pausar") : rem < t->duration_ms && !done ? _("Continuar") : _("Iniciar"));
    gtk_widget_set_sensitive(start, !done);
}

static void on_timer_start(GtkWidget *btn, gpointer data)
{
    (void)btn;
    clock_begin();
    XisTimer *t = xis_clock_find_timer(&g_clk, GPOINTER_TO_INT(data));
    if (!t) return;
    long long now = xis_clock_now_ms();
    if (t->end_ms > 0) {
        t->remaining_ms = xis_timer_remaining(t, now);
        t->end_ms = 0;
    } else {
        if (t->remaining_ms <= 0) t->remaining_ms = t->duration_ms;
        t->end_ms = now + t->remaining_ms;
    }
    clock_commit();
}

static void on_timer_reset(GtkWidget *btn, gpointer data)
{
    (void)btn;
    clock_begin();
    XisTimer *t = xis_clock_find_timer(&g_clk, GPOINTER_TO_INT(data));
    if (!t) return;
    t->end_ms = 0;
    t->remaining_ms = t->duration_ms;
    clock_commit();
}

static void on_timer_delete(GtkWidget *btn, gpointer data)
{
    (void)btn;
    clock_begin();
    xis_clock_remove_timer(&g_clk, GPOINTER_TO_INT(data));
    clock_commit();
}

static void on_timer_add(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    long long secs = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_timer_h)) * 3600LL +
                     gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_timer_m)) * 60LL +
                     gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_timer_s));
    if (secs <= 0) return;
    clock_begin();
    XisTimer *t = xis_clock_add_timer(&g_clk);
    if (!t) return;
    t->duration_ms = secs * 1000;
    t->remaining_ms = t->duration_ms;
    t->end_ms = xis_clock_now_ms() + t->duration_ms; /* starts right away */
    snprintf(t->label, sizeof(t->label), "%s", gtk_entry_get_text(GTK_ENTRY(g_timer_label)));
    gtk_entry_set_text(GTK_ENTRY(g_timer_label), "");
    clock_commit();
}

static void rebuild_timer_rows(void)
{
    clear_container(g_timer_list);
    long long now = xis_clock_now_ms();
    if (g_clk.ntimers == 0) {
        gtk_box_pack_start(GTK_BOX(g_timer_list), gtk_label_new(_("Nenhum temporizador.")), FALSE, FALSE, 8);
    }
    for (int i = 0; i < g_clk.ntimers; i++) {
        XisTimer *t = &g_clk.timers[i];
        GtkWidget *row = gtk_hbox_new(FALSE, 6);
        GtkWidget *label = gtk_label_new(NULL);
        gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
        GtkWidget *start = gtk_button_new_with_label(_("Iniciar"));
        g_signal_connect(start, "clicked", G_CALLBACK(on_timer_start), GINT_TO_POINTER(t->id));
        gtk_box_pack_start(GTK_BOX(row), start, FALSE, FALSE, 0);
        GtkWidget *reset = gtk_button_new_with_label(_("Zerar"));
        g_signal_connect(reset, "clicked", G_CALLBACK(on_timer_reset), GINT_TO_POINTER(t->id));
        gtk_box_pack_start(GTK_BOX(row), reset, FALSE, FALSE, 0);
        GtkWidget *del = gtk_button_new_with_label("\xc3\x97");
        xisserve_a11y(del, _("Excluir temporizador"), NULL);
        gtk_widget_set_tooltip_text(del, _("Excluir temporizador"));
        g_signal_connect(del, "clicked", G_CALLBACK(on_timer_delete), GINT_TO_POINTER(t->id));
        gtk_box_pack_start(GTK_BOX(row), del, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(row), "label", label);
        g_object_set_data(G_OBJECT(row), "start", start);
        g_object_set_data(G_OBJECT(row), "id", GINT_TO_POINTER(t->id));
        timer_row_update(t, now, row);
        gtk_box_pack_start(GTK_BOX(g_timer_list), row, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(g_timer_list);
}

static void update_timer_rows(void)
{
    long long now = xis_clock_now_ms();
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_timer_list));
    for (GList *l = children; l; l = l->next) {
        XisTimer *t = xis_clock_find_timer(&g_clk, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "id")));
        if (t) timer_row_update(t, now, GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

GtkWidget *clock_timers_build(void)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);

    g_timer_warning = warning_label();
    gtk_widget_set_no_show_all(g_timer_warning, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), g_timer_warning, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    g_timer_list = gtk_vbox_new(FALSE, 4);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), g_timer_list);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    GtkWidget *line1 = gtk_hbox_new(FALSE, 4);
    g_timer_h = time_spin(99, _("Horas"));
    g_timer_m = time_spin(59, _("Minutos"));
    g_timer_s = time_spin(59, _("Segundos"));
    gtk_box_pack_start(GTK_BOX(line1), g_timer_h, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line1), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line1), g_timer_m, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line1), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line1), g_timer_s, FALSE, FALSE, 0);
    g_timer_label = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(g_timer_label), XIS_CLOCK_LABEL_LEN - 1);
    xisserve_a11y(g_timer_label, _("Nome do temporizador"), NULL);
    gtk_widget_set_tooltip_text(g_timer_label, _("Nome do temporizador (opcional)"));
    gtk_box_pack_start(GTK_BOX(line1), g_timer_label, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), line1, FALSE, FALSE, 0);

    GtkWidget *line2 = gtk_hbox_new(FALSE, 4);
    GtkWidget *add = gtk_button_new_with_label(_("Iniciar temporizador"));
    g_signal_connect(add, "clicked", G_CALLBACK(on_timer_add), NULL);
    gtk_box_pack_end(GTK_BOX(line2), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), line2, FALSE, FALSE, 0);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_timer_m), 5);
    return vbox;
}

/* ---- refresh / tick --------------------------------------------------------- */

/* Structural refresh: after a load that may have added/removed rows. */
static void refresh_all(void)
{
    if (!g_alarm_list) return;
    rebuild_alarm_rows();
    rebuild_timer_rows();
    g_sw_shown_laps = -1;
    update_stopwatch();
}

static gboolean on_tick(gpointer data)
{
    (void)data;
    if (reload_if_changed()) {
        refresh_all();
        g_tick_id = 0; /* this source ends here; reschedule_tick() starts the next one */
        reschedule_tick();
        return FALSE;
    }
    switch (g_tab) {
    case CLOCK_TAB_ALARMS: update_alarm_rows(); break;
    case CLOCK_TAB_STOPWATCH: update_stopwatch(); break;
    case CLOCK_TAB_TIMERS: update_timer_rows(); break;
    case CLOCK_TAB_NONE: break;
    }
    return TRUE;
}

static void reschedule_tick(void)
{
    guint ms = 0;
    if (g_tab == CLOCK_TAB_STOPWATCH) {
        ms = g_clk.sw.start_ms > 0 ? 40 : 1000;
    } else if (g_tab == CLOCK_TAB_TIMERS) {
        ms = 1000;
        for (int i = 0; i < g_clk.ntimers; i++) {
            if (g_clk.timers[i].end_ms > 0) ms = 200; /* so seconds flip on time */
        }
    } else if (g_tab == CLOCK_TAB_ALARMS) {
        ms = 1000;
    }
    if (ms == g_tick_ms && g_tick_id) return;
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
    g_tick_ms = ms;
    if (ms) g_tick_id = g_timeout_add(ms, on_tick, NULL);
}

void clock_set_visible_tab(ClockTab tab)
{
    g_tab = tab;
    if (tab != CLOCK_TAB_NONE) {
        reload_if_changed();
        refresh_all();
    }
    reschedule_tick();
}

void clock_on_show(void)
{
    xis_clock_load(&g_clk);
    remember_mtime();
    gboolean warn = !kiconfd_running();
    gtk_widget_set_visible(g_alarm_warning, warn);
    gtk_widget_set_visible(g_timer_warning, warn);
    refresh_all();
}

void clock_on_hide(void)
{
    clock_set_visible_tab(CLOCK_TAB_NONE);
}
