/*
 * energy.c - the --energy page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section): AC/battery status, other UPower devices' batteries
 * (wireless mouse/keyboard, etc.), the screen backlight slider, and
 * night light.
 *
 * All state comes from power.c (`upower`/`brightnessctl`/`xsct`, no
 * linked library for any of the three -- see power.h). Each section is
 * only shown when its own backend is available, so a system missing one
 * tool still gets a useful page for the other two.
 *
 * Night light has two modes, both writing/reading the same
 * $XDG_CONFIG_HOME/kiconfd-nightlight.conf kiconf's Energia tab and
 * kiconfd itself use (see kiconfd.c's own doc comment for the schedule
 * side): the checkbox here toggles `enabled` between "kiconfd applies it
 * on the configured schedule" and "manual" -- in manual mode, dragging
 * the temperature slider calls xsct directly and immediately, the same
 * way the volume slider on --audio calls pactl directly. This page never
 * touches start/end times; those are kiconf's Energia tab's job.
 *
 * Refresh model: like --audio, a full rebuild on show plus a slow poll
 * while visible, so a laptop unplugging/plugging in or a peripheral's
 * battery draining shows up without reopening. Much slower than audio's
 * (battery percentage doesn't change tick to tick) and, like audio,
 * stands down while a slider is being dragged or shortly after a change
 * this page itself made.
 */
#include "../xisserve.h"
#include "power.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ENERGY_POLL_MS 5000
#define ENERGY_SETTLE_US (900 * 1000)
#define NIGHTLIGHT_TEMP_MIN 2700
#define NIGHTLIGHT_TEMP_MAX 6500
#define NIGHTLIGHT_TEMP_DEFAULT 4000

static GtkWidget *g_root;
static GtkWidget *g_box; /* rebuilt in place */
static guint g_poll_id;
static gboolean g_dragging;
static gint64 g_last_action_us;
static gboolean g_updating;

static GtkWidget *g_brightness_scale;
static GtkWidget *g_nightlight_chk;
static GtkWidget *g_nightlight_scale;

static void note_user_action(void)
{
    g_last_action_us = g_get_monotonic_time();
}

/* ---- kiconfd-nightlight.conf -------------------------------------------
 *
 * Shared with kiconf's Energia tab (which owns start/end) and kiconfd
 * (which applies the schedule) -- see kiconfd.c's own doc comment for the
 * full field list and reasoning. This page only ever reads/writes
 * `enabled` and `temp`, but round-trips every field it doesn't touch so
 * saving from here never clobbers a schedule set up in kiconf.
 */
typedef struct {
    int enabled;
    char start[8];
    char end[8];
    int temp;
} NightlightConfig;

static void nightlight_config_path(char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/kiconfd-nightlight.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(out, outsz, "%s/.config/kiconfd-nightlight.conf", home);
}

static void nightlight_config_defaults(NightlightConfig *c)
{
    c->enabled = 0;
    snprintf(c->start, sizeof(c->start), "20:00");
    snprintf(c->end, sizeof(c->end), "06:00");
    c->temp = NIGHTLIGHT_TEMP_DEFAULT;
}

static char *ltrim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void nightlight_config_load(NightlightConfig *c)
{
    nightlight_config_defaults(c);
    char path[PATH_MAX];
    nightlight_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *l = ltrim(line);
        size_t n = strlen(l);
        while (n > 0 && (l[n - 1] == '\n' || l[n - 1] == '\r')) {
            l[--n] = '\0';
        }
        if (!*l || *l == '#') {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = l;
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        char *end = eq;
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        char *val = ltrim(eq + 1);
        if (!strcmp(key, "enabled")) {
            c->enabled = atoi(val) ? 1 : 0;
        } else if (!strcmp(key, "start")) {
            snprintf(c->start, sizeof(c->start), "%s", val);
        } else if (!strcmp(key, "end")) {
            snprintf(c->end, sizeof(c->end), "%s", val);
        } else if (!strcmp(key, "temp")) {
            c->temp = atoi(val);
        }
    }
    fclose(f);
}

static void nightlight_config_save(const NightlightConfig *c)
{
    char path[PATH_MAX];
    nightlight_config_path(path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        return;
    }
    fprintf(f, "# kiconfd night light schedule -- see kiconf's Energia tab.\n");
    fprintf(f, "enabled = %d\n", c->enabled);
    fprintf(f, "start = %s\n", c->start);
    fprintf(f, "end = %s\n", c->end);
    fprintf(f, "temp = %d\n", c->temp);
    fclose(f);
    rename(tmp, path);
}

/* Same fork+execlp("pkill", "-HUP", "-x", ...) kiconf/common.c's own
 * signal_daemon() uses -- duplicated rather than shared, same as every
 * other cross-binary bit of this codebase (kiconf, kiconfd, xisserve and
 * kisession each build standalone, nothing here links against kiconf's
 * object files). -x, never -f: an -f pattern can match unrelated
 * processes by their full command line. */
static void notify_kiconfd(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execlp("pkill", "pkill", "-HUP", "-x", "kiconfd", (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
}

/* ---- widget callbacks ---------------------------------------------------- */

static gboolean on_brightness_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    g_dragging = TRUE;
    return FALSE;
}

static gboolean on_brightness_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    g_dragging = FALSE;
    note_user_action();
    return FALSE;
}

static void on_brightness_changed(GtkRange *range, gpointer data)
{
    (void)data;
    if (g_updating) {
        return;
    }
    note_user_action();
    brightness_set_pct((int)gtk_range_get_value(range));
}

static void on_nightlight_scale_changed(GtkRange *range, gpointer data)
{
    (void)data;
    if (g_updating) {
        return;
    }
    note_user_action();
    int temp = (int)gtk_range_get_value(range);
    nightlight_apply(temp);
    NightlightConfig c;
    nightlight_config_load(&c);
    c.temp = temp;
    nightlight_config_save(&c);
}

static void on_nightlight_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    if (g_updating) {
        return;
    }
    note_user_action();
    gboolean enabled = gtk_toggle_button_get_active(btn);
    NightlightConfig c;
    nightlight_config_load(&c);
    c.enabled = enabled;
    nightlight_config_save(&c);
    notify_kiconfd();
    gtk_widget_set_sensitive(g_nightlight_scale, !enabled);
    if (!enabled) {
        /* Handing manual control back -- apply whatever the slider is
         * already showing right away instead of waiting for the next
         * drag, so unchecking the box doesn't silently leave the
         * schedule's last temperature (or the day default) on screen. */
        nightlight_apply((int)gtk_range_get_value(GTK_RANGE(g_nightlight_scale)));
    }
}

/* ---- building ------------------------------------------------------------ */

static void style_fg(GtkWidget *w)
{
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    GdkColor c = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    static const GtkStateType states[] = {GTK_STATE_NORMAL, GTK_STATE_ACTIVE, GTK_STATE_PRELIGHT, GTK_STATE_SELECTED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        gtk_widget_modify_fg(w, states[i], &c);
        gtk_widget_modify_text(w, states[i], &c);
    }
}

static GtkWidget *section_header(const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    char markup[128];
    snprintf(markup, sizeof(markup), "<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    style_fg(label);
    return label;
}

static GtkWidget *plain_row(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    style_fg(label);
    return label;
}

static const char *state_label(PowerState s)
{
    switch (s) {
    case POWER_STATE_CHARGING: return "Carregando";
    case POWER_STATE_DISCHARGING: return "Descarregando";
    case POWER_STATE_FULL: return "Completa";
    default: return "";
    }
}

static void add_battery_section(void)
{
    gtk_box_pack_start(GTK_BOX(g_box), section_header("Energia"), FALSE, FALSE, 0);

    gboolean ac = power_ac_online();
    gtk_box_pack_start(GTK_BOX(g_box), plain_row(ac ? "Na tomada" : "Na bateria"), FALSE, FALSE, 0);

    PowerBattery bat;
    power_get_battery(&bat);
    if (bat.present) {
        char line[256];
        const char *st = state_label(bat.state);
        if (bat.time_text[0]) {
            snprintf(line, sizeof(line), "Bateria: %d%% (%s, %s restante)", bat.percentage, st, bat.time_text);
        } else {
            snprintf(line, sizeof(line), "Bateria: %d%% (%s)", bat.percentage, st);
        }
        gtk_box_pack_start(GTK_BOX(g_box), plain_row(line), FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(g_box), plain_row("Este computador nao tem bateria."), FALSE, FALSE, 0);
    }

    GPtrArray *peripherals = power_list_peripherals();
    if (peripherals->len > 0) {
        gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
        gtk_box_pack_start(GTK_BOX(g_box), section_header("Outros dispositivos"), FALSE, FALSE, 0);
        for (guint i = 0; i < peripherals->len; i++) {
            PowerPeripheral *p = g_ptr_array_index(peripherals, i);
            char line[256];
            snprintf(line, sizeof(line), "%s: %d%%", p->label, p->percentage);
            gtk_box_pack_start(GTK_BOX(g_box), plain_row(line), FALSE, FALSE, 0);
        }
    }
    power_peripherals_free(peripherals);
}

static void add_brightness_section(void)
{
    if (!brightness_available()) {
        return;
    }
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(g_box), section_header("Brilho da tela"), FALSE, FALSE, 0);

    g_brightness_scale = gtk_hscale_new_with_range(0, 100, 1);
    gtk_scale_set_digits(GTK_SCALE(g_brightness_scale), 0);
    gtk_scale_set_value_pos(GTK_SCALE(g_brightness_scale), GTK_POS_RIGHT);
    style_fg(g_brightness_scale);
    int pct = brightness_get_pct();
    g_updating = TRUE;
    gtk_range_set_value(GTK_RANGE(g_brightness_scale), pct >= 0 ? pct : 50);
    g_updating = FALSE;
    g_signal_connect(g_brightness_scale, "value-changed", G_CALLBACK(on_brightness_changed), NULL);
    g_signal_connect(g_brightness_scale, "button-press-event", G_CALLBACK(on_brightness_press), NULL);
    g_signal_connect(g_brightness_scale, "button-release-event", G_CALLBACK(on_brightness_release), NULL);
    gtk_box_pack_start(GTK_BOX(g_box), g_brightness_scale, FALSE, FALSE, 0);
}

static void add_nightlight_section(void)
{
    if (!nightlight_available()) {
        g_nightlight_chk = NULL;
        g_nightlight_scale = NULL;
        return;
    }
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(g_box), section_header("Luz noturna"), FALSE, FALSE, 0);

    NightlightConfig c;
    nightlight_config_load(&c);

    g_nightlight_chk = gtk_check_button_new_with_label("Automatica (horarios definidos em kiconf > Energia)");
    style_fg(g_nightlight_chk);
    style_fg(gtk_bin_get_child(GTK_BIN(g_nightlight_chk)));
    gtk_box_pack_start(GTK_BOX(g_box), g_nightlight_chk, FALSE, FALSE, 0);

    g_nightlight_scale = gtk_hscale_new_with_range(NIGHTLIGHT_TEMP_MIN, NIGHTLIGHT_TEMP_MAX, 100);
    gtk_scale_set_digits(GTK_SCALE(g_nightlight_scale), 0);
    gtk_scale_set_value_pos(GTK_SCALE(g_nightlight_scale), GTK_POS_RIGHT);
    style_fg(g_nightlight_scale);
    gtk_box_pack_start(GTK_BOX(g_box), g_nightlight_scale, FALSE, FALSE, 0);

    g_updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_nightlight_chk), c.enabled);
    gtk_range_set_value(GTK_RANGE(g_nightlight_scale), c.temp);
    g_updating = FALSE;
    gtk_widget_set_sensitive(g_nightlight_scale, !c.enabled);

    g_signal_connect(g_nightlight_chk, "toggled", G_CALLBACK(on_nightlight_toggled), NULL);
    g_signal_connect(g_nightlight_scale, "value-changed", G_CALLBACK(on_nightlight_scale_changed), NULL);
    g_signal_connect(g_nightlight_scale, "button-press-event", G_CALLBACK(on_brightness_press), NULL);
    g_signal_connect(g_nightlight_scale, "button-release-event", G_CALLBACK(on_brightness_release), NULL);
}

static void clear_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_box));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

static void rebuild(void)
{
    clear_rows();
    power_invalidate_available();
    brightness_invalidate_available();
    nightlight_invalidate_available();

    if (!power_available()) {
        gtk_box_pack_start(GTK_BOX(g_box),
                            plain_row("upower nao encontrado -- instale-o para ver bateria/energia aqui."),
                            FALSE, FALSE, 0);
    } else {
        add_battery_section();
    }
    add_brightness_section();
    add_nightlight_section();

    gtk_widget_show_all(g_box);
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    if (g_dragging || g_get_monotonic_time() - g_last_action_us < ENERGY_SETTLE_US) {
        return TRUE;
    }
    rebuild();
    return TRUE;
}

/* ---- page interface ------------------------------------------------------ */

GtkWidget *page_energy_build(void)
{
    g_box = gtk_vbox_new(FALSE, 6);

    g_root = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_root), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_root), g_box);

    GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(g_root));
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
    double r, g, b, a;
    xisserve_get_bg_rgba(&r, &g, &b, &a);
    GdkColor bg = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    gtk_widget_modify_bg(viewport, GTK_STATE_NORMAL, &bg);

    return g_root;
}

void page_energy_on_show(void)
{
    g_last_action_us = 0;
    g_dragging = FALSE;
    rebuild();
    if (!g_poll_id) {
        g_poll_id = g_timeout_add(ENERGY_POLL_MS, on_poll, NULL);
    }
}

void page_energy_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}
