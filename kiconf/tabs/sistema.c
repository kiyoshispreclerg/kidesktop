/* kiconf - Sistema tab: system language, date and time.
 * See kiconf.c's top doc comment for the overall design.
 *
 * Two backends, picked at runtime by have_systemd_timedate():
 *
 * - systemd present (localectl/timedatectl both on $PATH): `localectl
 *   set-locale`/`timedatectl set-*` change /etc/locale.conf and the
 *   RTC/NTP settings respectively, and both ask polkit to authorize the
 *   change themselves (systemd-localed's and systemd-timedated's own
 *   policy -- kiconf implements no privilege escalation of its own for
 *   this path, same subprocess-driven pattern as every other tab, just
 *   running a different command). That authorization has nowhere to
 *   prompt without a polkit agent in the session, which is why
 *   kisession's own "polkit" service defaults to on.
 *
 * - no systemd (Devuan's default sysvinit install, and any other
 *   non-systemd distro -- KiDesktop targets this as a first-class case,
 *   not systemd-only): plain Debian/POSIX tools that exist regardless of
 *   init system instead -- `update-locale`/`/etc/default/locale` for the
 *   locale, `/etc/localtime` + `/etc/timezone` + tzdata's zone1970.tab
 *   for the timezone, `date`/`hwclock` for manual time, and `service`/
 *   `update-rc.d` (LSB init-script wrappers present on virtually every
 *   sysvinit distro, not just Debian's) for toggling an NTP daemon.
 *   These need root the ordinary way (writing to /etc, running date/
 *   hwclock as root) since there's no privileged D-Bus service doing it
 *   on kiconf's behalf the way systemd-localed/timedated do -- so this
 *   path runs everything through `pkexec` instead, which is polkit's own
 *   privilege-escalation tool and just as init-system-agnostic as
 *   polkit itself: same net effect (polkit decides, its agent prompts if
 *   needed), just one layer more explicit. See run_privileged() below.
 *
 * Only the NTP-toggle side of the fallback is Devuan-specific in
 * practice (sysvinit's `service`/`update-rc.d`) -- an OpenRC or runit
 * system would need its own branch there (`rc-service`/`rc-update`,
 * `sv`) that nothing here implements yet; everything else (locale via
 * update-locale, timezone via zoneinfo) is genuinely init-independent.
 *
 * Deliberately minimal, per the brief: just LANG= and the clock, not
 * LC_* per-category overrides or keyboard layout (Entrada's X11 layout
 * fields are a separate, X-specific thing from localectl's console/X11
 * keymap commands, and not folded in here). The language combo only
 * offers locales already known to the system (`localectl list-locales`,
 * or `locale -a` without systemd) -- generating a new one (Debian/
 * Ubuntu's locale-gen, or equivalent) is a distro-specific step outside
 * kiconf's reach either way. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GtkWidget *g_status_label;
static GtkWidget *g_locale_combo;
static GtkWidget *g_tz_combo;
static GtkWidget *g_ntp_chk;
static GtkWidget *g_year_spin, *g_month_spin, *g_day_spin, *g_hour_spin, *g_min_spin, *g_sec_spin;
static GtkWidget *g_manual_box;

/* Forward declarations: the non-systemd fallback helpers live further
 * down (see "non-systemd fallbacks" below), but current_locale() and
 * apply_cb() above that section need to call into them. */
static int have_systemd_timedate(void);
static int run_privileged(char *const argv[]);
static int is_safe_token(const char *s);
static void current_locale_fallback(char *out, size_t outsz);
static void current_timezone_fallback(char *out, size_t outsz);
static void fill_tz_combo_fallback(GtkWidget *combo, const char *current);
static const char *detect_ntp_service(void);

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
static void current_locale_systemd(char *out, size_t outsz)
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

static void current_locale(char *out, size_t outsz)
{
    if (have_systemd_timedate()) {
        current_locale_systemd(out, outsz);
    } else {
        current_locale_fallback(out, outsz);
    }
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

/* The one flag every function below branches on -- see the file doc
 * comment. Both binaries are systemd-only, so their absence is as good a
 * "no systemd here" signal as actually checking `ps 1`/pid 1's comm. */
static int have_systemd_timedate(void)
{
    return have_cmd("localectl") && have_cmd("timedatectl");
}

/* Runs argv as root via pkexec -- the fallback path's stand-in for the
 * privileged D-Bus call localectl/timedatectl make on kiconf's behalf
 * under systemd (see the file doc comment). polkit itself doesn't care
 * whether systemd is PID 1, so this prompts (or not) exactly the same
 * way the systemd path's own polkit check would. */
static int run_privileged(char *const argv[])
{
    char *pk_argv[16];
    int i = 0;
    pk_argv[i++] = (char *)"pkexec";
    for (int j = 0; argv[j] && i < 15; j++) {
        pk_argv[i++] = argv[j];
    }
    pk_argv[i] = NULL;
    return run_fire(pk_argv);
}

/* Only letters/digits/'/','_','+','-','.' -- every real zoneinfo/locale
 * name fits this, and it's cheap insurance against the values below ever
 * reaching a `sh -c` string unescaped (they only ever come from our own
 * combo lists today, never free-typed, but this costs nothing). */
static int is_safe_token(const char *s)
{
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr("/_+-.", *s)) {
            return 0;
        }
    }
    return *s == '\0';
}

/* ---- non-systemd fallbacks (Devuan et al.) ------------------------------ */

/* Debian/Devuan's `update-locale` writes /etc/default/locale, which is
 * also where the *current* LANG lives when there's no systemd-localed to
 * ask -- read it straight, falling back to the environment (set by
 * whatever session startup already sourced that file into) if the file
 * itself is missing or unreadable for some reason. */
static void current_locale_fallback(char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/default/locale", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *l = trim(line);
            if (!strncmp(l, "LANG=", 5)) {
                char *val = l + 5;
                size_t n = strlen(val);
                if (n >= 2 && val[0] == '"' && val[n - 1] == '"') {
                    val[n - 1] = '\0';
                    val++;
                }
                snprintf(out, outsz, "%s", val);
                break;
            }
        }
        fclose(f);
    }
    if (!out[0]) {
        const char *env = getenv("LANG");
        if (env) {
            snprintf(out, outsz, "%s", env);
        }
    }
}

/* /etc/timezone (Debian/Devuan's own plain-text record) is the fast
 * path; falling back to resolving /etc/localtime's symlink target
 * against /usr/share/zoneinfo/ covers non-Debian non-systemd distros
 * that don't keep /etc/timezone at all (the symlink itself is the one
 * thing every tzdata-based system agrees on). */
static void current_timezone_fallback(char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/timezone", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            snprintf(out, outsz, "%s", trim(line));
        }
        fclose(f);
    }
    if (out[0]) {
        return;
    }
    char link[PATH_MAX];
    ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (n <= 0) {
        return;
    }
    link[n] = '\0';
    const char *prefix = "/usr/share/zoneinfo/";
    const char *p = strstr(link, prefix);
    if (p) {
        snprintf(out, outsz, "%s", p + strlen(prefix));
    }
}

/* tzdata's own zone1970.tab (falling back to the older zone.tab on a
 * system too old to ship it) is the exact same list systemd-timedated
 * uses for `timedatectl list-timezones` -- both just read this file, so
 * the fallback combo ends up with the identical set of names. Column 3
 * (0-indexed 2) of each non-comment line is the zone name; the other
 * columns (country codes, coordinates, an optional trailing comment)
 * aren't shown anywhere in this tab so they're simply discarded here. */
static void fill_tz_combo_fallback(GtkWidget *combo, const char *current)
{
    FILE *f = fopen("/usr/share/zoneinfo/zone1970.tab", "r");
    if (!f) {
        f = fopen("/usr/share/zoneinfo/zone.tab", "r");
    }
    if (!f) {
        if (current && *current) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(combo), current);
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
        }
        return;
    }
    int idx = -1, i = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        char *save = NULL;
        strtok_r(line, "\t", &save);          /* country codes, discarded */
        char *rest = strtok_r(NULL, "\t", &save);
        (void)rest;                            /* coordinates, discarded */
        char *zone = strtok_r(NULL, "\t\n", &save);
        if (!zone || !*zone) {
            continue;
        }
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), zone);
        if (current && !strcmp(zone, current)) {
            idx = i;
        }
        i++;
    }
    fclose(f);
    if (idx < 0 && current && *current) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), current);
        idx = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx >= 0 ? idx : 0);
}

/* Which init-script name to drive for the NTP checkbox -- first one
 * actually installed wins. Covers the two packages Devuan itself
 * suggests for time sync (chrony, the classic `ntp` package); openntpd
 * is a common third choice elsewhere. All three ship a plain
 * /etc/init.d/<name> LSB script under sysvinit, which is what
 * service/update-rc.d below drive either way. */
static const char *detect_ntp_service(void)
{
    static const char *candidates[] = {"chrony", "ntp", "openntpd", NULL};
    static char path[64];
    for (int i = 0; candidates[i]; i++) {
        snprintf(path, sizeof(path), "/etc/init.d/%s", candidates[i]);
        if (access(path, F_OK) == 0) {
            return candidates[i];
        }
    }
    return NULL;
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

    int systemd_time = have_systemd_timedate();
    GString *msg = g_string_new(NULL);

    gchar *locale = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_locale_combo));
    if (locale && *locale) {
        int ok;
        if (systemd_time) {
            char arg[NAME_LEN + 8];
            snprintf(arg, sizeof(arg), "LANG=%s", locale);
            char *argv[] = {"localectl", "set-locale", arg, NULL};
            ok = run_fire(argv) == 0;
        } else {
            char arg[NAME_LEN + 8];
            snprintf(arg, sizeof(arg), "LANG=%s", locale);
            /* update-locale (from the `locales` package) is the Debian/
             * Devuan tool for exactly this -- writes /etc/default/locale,
             * same file current_locale_fallback() reads back. */
            char *argv[] = {"update-locale", arg, NULL};
            ok = have_cmd("update-locale") && run_privileged(argv) == 0;
        }
        g_string_append(msg, ok ? "Idioma aplicado (vale para novas sessoes/aplicativos). "
                                 : "Falha ao aplicar idioma (polkit/permissao?). ");
    }
    g_free(locale);

    gchar *tz = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_tz_combo));
    if (tz && *tz && is_safe_token(tz)) {
        int ok;
        if (systemd_time) {
            char *argv[] = {"timedatectl", "set-timezone", tz, NULL};
            ok = run_fire(argv) == 0;
        } else {
            /* The canonical non-systemd way (works with or without
             * Debian's dpkg-reconfigure, see the file doc comment):
             * /etc/localtime is the symlink every tzdata-using program
             * actually reads, /etc/timezone is Debian/Devuan's own
             * plain-text record of the same fact -- dpkg-reconfigure, if
             * present, additionally rebuilds tzdata's binary cache. */
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "printf '%%s\\n' '%s' > /etc/timezone && ln -sf '/usr/share/zoneinfo/%s' /etc/localtime%s",
                     tz, tz,
                     have_cmd("dpkg-reconfigure") ? " && dpkg-reconfigure -f noninteractive tzdata >/dev/null 2>&1"
                                                   : "");
            char *argv[] = {"sh", "-c", cmd, NULL};
            ok = run_privileged(argv) == 0;
        }
        g_string_append(msg, ok ? "Fuso horario aplicado. " : "Falha ao aplicar fuso horario. ");
    }
    g_free(tz);

    int ntp_on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_ntp_chk));
    if (systemd_time) {
        char *ntp_argv[] = {"timedatectl", "set-ntp", ntp_on ? "true" : "false", NULL};
        run_fire(ntp_argv);
        g_string_append(msg, ntp_on ? "Hora sincronizada por rede. " : "Sincronizacao por rede desligada. ");
    } else {
        const char *svc = detect_ntp_service();
        if (svc) {
            char *argv[] = {"service", (char *)svc, ntp_on ? "start" : "stop", NULL};
            run_privileged(argv);
            if (have_cmd("update-rc.d")) {
                char *rc_argv[] = {"update-rc.d", (char *)svc, ntp_on ? "enable" : "disable", NULL};
                run_privileged(rc_argv);
            }
            g_string_append(msg, ntp_on ? "Hora sincronizada por rede (" : "Sincronizacao por rede desligada (");
            g_string_append(msg, svc);
            g_string_append(msg, "). ");
        } else {
            g_string_append(msg, "Nenhum servico de NTP conhecido (chrony/ntp/openntpd) instalado -- "
                                  "instale um deles para sincronizar a hora pela rede. ");
        }
    }

    if (!ntp_on) {
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_year_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_month_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_day_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_hour_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_min_spin)),
                 gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sec_spin)));
        int ok;
        if (systemd_time) {
            char *argv[] = {"timedatectl", "set-time", stamp, NULL};
            ok = run_fire(argv) == 0;
        } else {
            /* `date -s` sets the kernel clock; hwclock additionally
             * writes it to the RTC so it survives a reboot -- systemd-
             * timedated's set-time does both in one call, this needs the
             * two commands spelled out. */
            char *argv[] = {"date", "-s", stamp, NULL};
            ok = run_privileged(argv) == 0;
            if (ok && have_cmd("hwclock")) {
                char *hw_argv[] = {"hwclock", "--systohc", NULL};
                run_privileged(hw_argv);
            }
        }
        g_string_append(msg, ok ? "Data/hora ajustadas."
                                 : "Falha ao ajustar data/hora (desligue a sincronizacao por rede antes).");
    }

    gtk_label_set_text(GTK_LABEL(g_status_label), msg->str);
    g_string_free(msg, TRUE);
}

GtkWidget *build_sistema_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    int systemd_time = have_systemd_timedate();
    const char *ntp_svc = systemd_time ? NULL : detect_ntp_service();
    const char *status_text;
    if (systemd_time) {
        status_text = "";
    } else if (!have_cmd("update-locale") && access("/usr/share/zoneinfo", F_OK) != 0) {
        /* Genuinely nothing to work with -- not even the Debian/POSIX
         * fallback tools this tab otherwise relies on. */
        status_text = "Nem localectl/timedatectl (systemd) nem update-locale/tzdata foram "
                      "encontrados -- esta aba nao tem como funcionar neste sistema.";
    } else {
        status_text = "Sistema sem systemd detectado -- usando update-locale/tzdata "
                      "(via pkexec) em vez de localectl/timedatectl.";
    }
    g_status_label = gtk_label_new(status_text);
    gtk_misc_set_alignment(GTK_MISC(g_status_label), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), g_status_label, FALSE, FALSE, 0);

    /* Idioma */
    GtkWidget *lang_table = gtk_table_new(1, 2, FALSE);
    g_locale_combo = gtk_combo_box_new_text();
    char cur_locale[NAME_LEN];
    current_locale(cur_locale, sizeof(cur_locale));
    if (systemd_time) {
        fill_combo_from_lines(g_locale_combo, (char *[]){"localectl", "list-locales", NULL}, cur_locale);
    } else {
        /* `locale -a` needs no root and exists on every glibc system
         * regardless of init -- same list localectl itself would show,
         * minus its own normalization of the raw glibc output. */
        fill_combo_from_lines(g_locale_combo, (char *[]){"locale", "-a", NULL}, cur_locale);
    }
    labeled_row(lang_table, 0, "Idioma do sistema (LANG):", g_locale_combo);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Idioma", lang_table), FALSE, FALSE, 0);

    /* Data e hora */
    GtkWidget *dt_vbox = gtk_vbox_new(FALSE, 6);

    GtkWidget *tz_table = gtk_table_new(1, 2, FALSE);
    g_tz_combo = gtk_combo_box_new_text();
    char cur_tz[NAME_LEN];
    if (systemd_time) {
        timedatectl_show_get("Timezone", cur_tz, sizeof(cur_tz));
        fill_combo_from_lines(g_tz_combo, (char *[]){"timedatectl", "list-timezones", NULL}, cur_tz);
    } else {
        current_timezone_fallback(cur_tz, sizeof(cur_tz));
        fill_tz_combo_fallback(g_tz_combo, cur_tz);
    }
    labeled_row(tz_table, 0, "Fuso horario:", g_tz_combo);
    gtk_box_pack_start(GTK_BOX(dt_vbox), tz_table, FALSE, FALSE, 0);

    int ntp_on;
    if (systemd_time) {
        char ntp_val[16];
        ntp_on = timedatectl_show_get("NTP", ntp_val, sizeof(ntp_val)) && !strcmp(ntp_val, "yes");
    } else {
        /* No systemd-timedated to ask "is NTP on" -- whether the
         * detected daemon (see detect_ntp_service()) is actually running
         * right now is the closest equivalent without one. */
        ntp_on = ntp_svc && process_running(ntp_svc);
    }
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
