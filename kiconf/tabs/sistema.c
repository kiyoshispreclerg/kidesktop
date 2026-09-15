/* kiconf - Sistema tab: system language, date and time.
 * See kiconf.c's top doc comment for the overall design.
 *
 * Both are systemd-owned: `localectl set-locale`/`timedatectl set-*`
 * change /etc/locale.conf and the RTC/NTP settings respectively, and both
 * ask polkit to authorize the change themselves (systemd-localed's and
 * systemd-timedated's own policy -- kiconf implements no privilege
 * escalation of its own, same subprocess-driven pattern as every other
 * tab, just running a different command). That authorization has nowhere
 * to prompt without a polkit agent in the session, which is why
 * kisession's own "polkit" service defaults to on.
 *
 * Deliberately minimal, per the brief: just LANG= and the clock, not
 * LC_* per-category overrides or keyboard layout (Entrada's X11 layout
 * fields are a separate, X-specific thing from localectl's console/X11
 * keymap commands, and not folded in here). The language combo only
 * offers locales `localectl list-locales` already knows about --
 * generating a new one (Debian/Ubuntu's locale-gen, or equivalent) is a
 * distro-specific step outside kiconf's reach. */
#include "../common.h"
#include "../tabs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GtkWidget *g_status_label;
static GtkWidget *g_locale_combo;
static GtkWidget *g_tz_combo;
static GtkWidget *g_ntp_chk;
static GtkWidget *g_year_spin, *g_month_spin, *g_day_spin, *g_hour_spin, *g_min_spin, *g_sec_spin;
static GtkWidget *g_manual_box;

/* Runs argv and appends every output line to `combo` (a plain text
 * combobox), trimmed of its trailing newline -- localectl/timedatectl's
 * list-* subcommands are one entry per line, nothing else to parse. */
static void fill_combo_from_lines(GtkWidget *combo, char *const argv[], const char *current)
{
    char out[65536];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    int idx = -1, i = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), line);
        if (current && !strcmp(line, current)) {
            idx = i;
        }
        i++;
    }
    if (idx < 0 && current && *current) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), current);
        idx = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx >= 0 ? idx : 0);
}

/* "   System Locale: LANG=pt_BR.UTF-8" (localectl status's first line,
 * possibly followed by other LC_-category/LANGUAGE lines this tab
 * doesn't touch) -> "pt_BR.UTF-8". No `localectl show`-style machine
 * output exists for locale the way timedatectl has one, so this scrapes
 * the human one. */
static void current_locale(char *out, size_t outsz)
{
    out[0] = '\0';
    char *argv[] = {"localectl", "status", NULL};
    char status[8192];
    if (!run_capture(argv, status, sizeof(status))) {
        return;
    }
    char *p = strstr(status, "LANG=");
    if (!p) {
        return;
    }
    p += 5;
    char *end = p;
    while (*end && *end != '\n' && *end != '\r') {
        end++;
    }
    size_t n = (size_t)(end - p);
    if (n >= outsz) {
        n = outsz - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* timedatectl show's Key=Value lines, one per line, machine-readable
 * (unlike `status`) -- exactly what this needs for Timezone=/NTP=. */
static int timedatectl_show_get(const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    char *argv[] = {"timedatectl", "show", NULL};
    char show[8192];
    if (!run_capture(argv, show, sizeof(show))) {
        return 0;
    }
    size_t keylen = strlen(key);
    char *save = NULL;
    for (char *line = strtok_r(show, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, key, keylen) && line[keylen] == '=') {
            snprintf(out, outsz, "%s", line + keylen + 1);
            return 1;
        }
    }
    return 0;
}

static void set_now_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GDateTime *now = g_date_time_new_now_local();
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_year_spin), g_date_time_get_year(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_month_spin), g_date_time_get_month(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_day_spin), g_date_time_get_day_of_month(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_hour_spin), g_date_time_get_hour(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_min_spin), g_date_time_get_minute(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sec_spin), g_date_time_get_second(now));
    g_date_time_unref(now);
}

static int have_cmd(const char *name)
{
    char out[PATH_MAX];
    char *argv[] = {"which", (char *)name, NULL};
    return run_capture(argv, out, sizeof(out)) && out[0];
}

static void ntp_toggled_cb(GtkWidget *widget, gpointer data)
{
    (void)data;
    gtk_widget_set_sensitive(g_manual_box, !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

static void apply_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    GString *msg = g_string_new(NULL);

    gchar *locale = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_locale_combo));
    if (locale && *locale) {
        char arg[NAME_LEN + 8];
        snprintf(arg, sizeof(arg), "LANG=%s", locale);
        char *argv[] = {"localectl", "set-locale", arg, NULL};
        if (run_fire(argv) == 0) {
            g_string_append(msg, "Idioma aplicado (vale para novas sessoes/aplicativos). ");
        } else {
            g_string_append(msg, "Falha ao aplicar idioma (polkit/permissao?). ");
        }
    }
    g_free(locale);

    gchar *tz = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_tz_combo));
    if (tz && *tz) {
        char *argv[] = {"timedatectl", "set-timezone", tz, NULL};
        if (run_fire(argv) == 0) {
            g_string_append(msg, "Fuso horario aplicado. ");
        } else {
            g_string_append(msg, "Falha ao aplicar fuso horario. ");
        }
    }
    g_free(tz);

    int ntp_on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_ntp_chk));
    char *ntp_argv[] = {"timedatectl", "set-ntp", ntp_on ? "true" : "false", NULL};
    run_fire(ntp_argv);
    g_string_append(msg, ntp_on ? "Hora sincronizada por rede. " : "Sincronizacao por rede desligada. ");

    if (!ntp_on) {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_year_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_month_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_day_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_hour_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_min_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sec_spin)));
        char *argv[] = {"timedatectl", "set-time", stamp, NULL};
        if (run_fire(argv) == 0) {
            g_string_append(msg, "Data/hora ajustadas.");
        } else {
            g_string_append(msg, "Falha ao ajustar data/hora (desligue a sincronizacao por rede antes).");
        }
    }

    gtk_label_set_text(GTK_LABEL(g_status_label), msg->str);
    g_string_free(msg, TRUE);
}

GtkWidget *build_sistema_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_status_label = gtk_label_new(
        (!have_cmd("localectl") || !have_cmd("timedatectl"))
            ? "localectl/timedatectl nao encontrados -- esta aba precisa do systemd."
            : "");
    gtk_misc_set_alignment(GTK_MISC(g_status_label), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), g_status_label, FALSE, FALSE, 0);

    /* Idioma */
    GtkWidget *lang_table = gtk_table_new(1, 2, FALSE);
    g_locale_combo = gtk_combo_box_new_text();
    char cur_locale[NAME_LEN];
    current_locale(cur_locale, sizeof(cur_locale));
    fill_combo_from_lines(g_locale_combo, (char *[]){"localectl", "list-locales", NULL}, cur_locale);
    labeled_row(lang_table, 0, "Idioma do sistema (LANG):", g_locale_combo);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Idioma", lang_table), FALSE, FALSE, 0);

    /* Data e hora */
    GtkWidget *dt_vbox = gtk_vbox_new(FALSE, 6);

    GtkWidget *tz_table = gtk_table_new(1, 2, FALSE);
    g_tz_combo = gtk_combo_box_new_text();
    char cur_tz[NAME_LEN];
    timedatectl_show_get("Timezone", cur_tz, sizeof(cur_tz));
    fill_combo_from_lines(g_tz_combo, (char *[]){"timedatectl", "list-timezones", NULL}, cur_tz);
    labeled_row(tz_table, 0, "Fuso horario:", g_tz_combo);
    gtk_box_pack_start(GTK_BOX(dt_vbox), tz_table, FALSE, FALSE, 0);

    char ntp_val[16];
    int ntp_on = timedatectl_show_get("NTP", ntp_val, sizeof(ntp_val)) && !strcmp(ntp_val, "yes");
    g_ntp_chk = gtk_check_button_new_with_label("Sincronizar hora automaticamente pela rede (NTP)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ntp_chk), ntp_on);
    g_signal_connect(g_ntp_chk, "toggled", G_CALLBACK(ntp_toggled_cb), NULL);
    gtk_box_pack_start(GTK_BOX(dt_vbox), g_ntp_chk, FALSE, FALSE, 0);

    GDateTime *now = g_date_time_new_now_local();
    g_manual_box = gtk_hbox_new(FALSE, 4);
    g_year_spin = gtk_spin_button_new_with_range(1970, 2200, 1);
    g_month_spin = gtk_spin_button_new_with_range(1, 12, 1);
    g_day_spin = gtk_spin_button_new_with_range(1, 31, 1);
    g_hour_spin = gtk_spin_button_new_with_range(0, 23, 1);
    g_min_spin = gtk_spin_button_new_with_range(0, 59, 1);
    g_sec_spin = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_year_spin), g_date_time_get_year(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_month_spin), g_date_time_get_month(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_day_spin), g_date_time_get_day_of_month(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_hour_spin), g_date_time_get_hour(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_min_spin), g_date_time_get_minute(now));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sec_spin), g_date_time_get_second(now));
    g_date_time_unref(now);
    gtk_box_pack_start(GTK_BOX(g_manual_box), gtk_label_new("Data:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), g_year_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), g_month_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), g_day_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), gtk_label_new("Hora:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), g_hour_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), g_min_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_manual_box), g_sec_spin, TRUE, TRUE, 0);
    GtkWidget *now_btn = gtk_button_new_with_label("Agora");
    g_signal_connect(now_btn, "clicked", G_CALLBACK(set_now_cb), NULL);
    gtk_box_pack_start(GTK_BOX(g_manual_box), now_btn, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(g_manual_box, !ntp_on);
    gtk_box_pack_start(GTK_BOX(dt_vbox), g_manual_box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), frame_with("Data e hora", dt_vbox), FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(apply_cb), NULL);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    return outer;
}
