/* kiconf - Login tab: the display manager's session/autologin settings,
 * for SDDM, LightDM and SLiM behind one form.
 *
 * Each manager is a small backend: where its config lives, how the common
 * fields map onto its keys, and which file a change is written to. The
 * form is shared; a field a manager has no key for is greyed out.
 *
 *   SDDM    INI. [Autologin] User/Session, [General] Numlock, [Theme] Current
 *   LightDM INI. [Seat:*] autologin-user, autologin-session, user-session,
 *           greeter-session
 *   SLiM    "key value". auto_login, default_user, sessions, numlock,
 *           current_theme
 *
 * Reading merges the files in the order the manager itself does (drop-in
 * directories first, the main file last, later wins) and needs no
 * privileges. Writing edits the file that wins -- the main file if it
 * exists, else a zz-kiconf.conf drop-in that sorts after the others --
 * in memory, and hands the whole result to ONE pkexec'd script on stdin
 * (never a temp file another process could swap). The script only knows
 * a fixed list of destinations, keeps a *.kiconf-bak of the original and
 * replaces the file atomically. The manager is not restarted: that
 * would kill the running session, so changes apply at the next login. */
#include "../common.h"
#include "../tabs.h"

#include <dirent.h>
#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum { DM_SDDM, DM_LIGHTDM, DM_SLIM, N_DM } Dm;

typedef struct {
    int autologin;
    char user[64];
    char session[96];
    char numlock[8];  /* "", "on", "off" ("" = leave as is) */
    char theme[96];
} Cfg;

static const struct {
    const char *id, *label, *bin, *theme_label;
} DMS[N_DM] = {
    { "sddm", "SDDM", "sddm", N_("Tema:") },
    { "lightdm", "LightDM", "lightdm", N_("Greeter:") },
    { "slim", "SLiM", "slim", N_("Tema:") },
};

static const char SCRIPT[] =
    "set -e\n"
    "case \"$1/$2\" in\n"
    "  sddm/main) dst=/etc/sddm.conf;;\n"
    "  sddm/dropin) mkdir -p /etc/sddm.conf.d; dst=/etc/sddm.conf.d/zz-kiconf.conf;;\n"
    "  lightdm/main) dst=/etc/lightdm/lightdm.conf;;\n"
    "  lightdm/dropin) mkdir -p /etc/lightdm/lightdm.conf.d; dst=/etc/lightdm/lightdm.conf.d/zz-kiconf.conf;;\n"
    "  slim/main) dst=/etc/slim.conf;;\n"
    "  *) exit 2;;\n"
    "esac\n"
    "if [ -e \"$dst\" ] && [ ! -e \"$dst.kiconf-bak\" ]; then cp -p \"$dst\" \"$dst.kiconf-bak\"; fi\n"
    "cat > \"$dst.kiconf-new\"\n"
    "chmod 644 \"$dst.kiconf-new\"\n"
    "mv \"$dst.kiconf-new\" \"$dst\"\n";

static Dm g_dm;
static GtkWidget *g_dm_combo, *g_auto_chk, *g_user_combo, *g_session_combo, *g_numlock_combo,
    *g_theme_combo, *g_status, *g_hint;
static int g_dm_avail[N_DM];
static Dm g_combo_dm[N_DM];
static int g_ncombo;

/* ---- reading a config chain ------------------------------------------------ */

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The files of `dir` ending in .conf, sorted, appended to `list`. */
static void add_dir(GPtrArray *list, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    GPtrArray *names = g_ptr_array_new();
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 5 && !strcmp(e->d_name + n - 5, ".conf")) {
            g_ptr_array_add(names, g_strdup_printf("%s/%s", dir, e->d_name));
        }
    }
    closedir(d);
    qsort(names->pdata, names->len, sizeof(char *), str_cmp);
    for (guint i = 0; i < names->len; i++) {
        g_ptr_array_add(list, names->pdata[i]);
    }
    g_ptr_array_free(names, FALSE);
}

/* Files in precedence order, lowest first. */
static GPtrArray *config_chain(Dm dm)
{
    GPtrArray *l = g_ptr_array_new_with_free_func(g_free);
    switch (dm) {
    case DM_SDDM:
        add_dir(l, "/usr/lib/sddm/sddm.conf.d");
        add_dir(l, "/etc/sddm.conf.d");
        g_ptr_array_add(l, g_strdup("/etc/sddm.conf"));
        break;
    case DM_LIGHTDM:
        add_dir(l, "/usr/share/lightdm/lightdm.conf.d");
        add_dir(l, "/etc/lightdm/lightdm.conf.d");
        g_ptr_array_add(l, g_strdup("/etc/lightdm/lightdm.conf"));
        break;
    default:
        g_ptr_array_add(l, g_strdup("/etc/slim.conf"));
        break;
    }
    return l;
}

/* The path writes go to, and the script's "main"/"dropin" word for it. */
static const char *write_mode(Dm dm)
{
    if (dm == DM_SLIM) {
        return "main";
    }
    const char *main_file = dm == DM_SDDM ? "/etc/sddm.conf" : "/etc/lightdm/lightdm.conf";
    return access(main_file, F_OK) == 0 ? "main" : "dropin";
}

static char *write_target(Dm dm)
{
    if (!strcmp(write_mode(dm), "main")) {
        return g_strdup(dm == DM_SDDM ? "/etc/sddm.conf" : dm == DM_SLIM ? "/etc/slim.conf" : "/etc/lightdm/lightdm.conf");
    }
    return g_strdup(dm == DM_SDDM ? "/etc/sddm.conf.d/zz-kiconf.conf" : "/etc/lightdm/lightdm.conf.d/zz-kiconf.conf");
}

/* Splits `line` into key and value of "key = value" (INI) or "key value"
 * (SLiM). Returns NULL for comments, blanks and section headers. */
static char *line_key(const char *line, int ini, const char **val)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (!*p || *p == '#' || *p == ';' || *p == '[') {
        return NULL;
    }
    const char *k = p;
    while (*p && *p != '=' && !(!ini && (*p == ' ' || *p == '\t'))) {
        p++;
    }
    const char *kend = p;
    while (kend > k && (kend[-1] == ' ' || kend[-1] == '\t')) {
        kend--;
    }
    if (ini) {
        if (*p != '=') {
            return NULL;
        }
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *val = p;
    return g_strndup(k, (gsize)(kend - k));
}

static void trim_eol(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* Last definition of `key` (in `section` for INI) across the chain. */
static int chain_get(Dm dm, const char *section, const char *key, char *out, size_t outsz)
{
    int ini = dm != DM_SLIM, found = 0;
    GPtrArray *chain = config_chain(dm);
    for (guint i = 0; i < chain->len; i++) {
        char *text = NULL;
        if (!g_file_get_contents(chain->pdata[i], &text, NULL, NULL)) {
            continue;
        }
        char **lines = g_strsplit(text, "\n", -1);
        int in_sec = !ini;
        for (char **l = lines; *l; l++) {
            const char *t = *l;
            while (*t == ' ' || *t == '\t') {
                t++;
            }
            if (ini && *t == '[') {
                char hdr[96];
                snprintf(hdr, sizeof(hdr), "[%s]", section);
                in_sec = !strncmp(t, hdr, strlen(hdr));
                continue;
            }
            const char *val;
            char *k = in_sec ? line_key(*l, ini, &val) : NULL;
            if (k && !strcmp(k, key)) {
                snprintf(out, outsz, "%s", val);
                trim_eol(out);
                found = 1;
            }
            g_free(k);
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_ptr_array_free(chain, TRUE);
    return found;
}

static void read_cfg(Dm dm, Cfg *c)
{
    memset(c, 0, sizeof(*c));
    char v[512];
    switch (dm) {
    case DM_SDDM:
        if (chain_get(dm, "Autologin", "User", c->user, sizeof(c->user))) {
            c->autologin = c->user[0] != '\0';
        }
        chain_get(dm, "Autologin", "Session", c->session, sizeof(c->session));
        if (chain_get(dm, "General", "Numlock", v, sizeof(v)) && (!strcmp(v, "on") || !strcmp(v, "off"))) {
            snprintf(c->numlock, sizeof(c->numlock), "%.3s", v);
        }
        chain_get(dm, "Theme", "Current", c->theme, sizeof(c->theme));
        break;
    case DM_LIGHTDM:
        if (chain_get(dm, "Seat:*", "autologin-user", c->user, sizeof(c->user))) {
            c->autologin = c->user[0] != '\0';
        }
        if (!chain_get(dm, "Seat:*", "user-session", c->session, sizeof(c->session))) {
            chain_get(dm, "Seat:*", "autologin-session", c->session, sizeof(c->session));
        }
        chain_get(dm, "Seat:*", "greeter-session", c->theme, sizeof(c->theme));
        break;
    default:
        if (chain_get(dm, "", "auto_login", v, sizeof(v))) {
            c->autologin = !strcmp(v, "yes");
        }
        chain_get(dm, "", "default_user", c->user, sizeof(c->user));
        if (chain_get(dm, "", "sessions", v, sizeof(v))) {
            char *comma = strchr(v, ',');
            if (comma) {
                *comma = '\0';
            }
            snprintf(c->session, sizeof(c->session), "%.95s", v);
        }
        if (chain_get(dm, "", "numlock", v, sizeof(v)) && (!strcmp(v, "on") || !strcmp(v, "off"))) {
            snprintf(c->numlock, sizeof(c->numlock), "%.3s", v);
        }
        chain_get(dm, "", "current_theme", c->theme, sizeof(c->theme));
        break;
    }
}

/* ---- editing the text of one file ---------------------------------------- */

/* Sets `key` to `value` in `section` ("" for SLiM) of `lines` (a NULL-
 * terminated GPtrArray of line strings, no newlines). */
static void text_set(GPtrArray *lines, int ini, const char *section, const char *key, const char *value)
{
    char *newline = ini ? g_strdup_printf("%s=%s", key, value) : g_strdup_printf("%-15s %s", key, value);
    int sec_start = ini ? -1 : 0, sec_end = (int)lines->len;
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "[%s]", section);
    if (ini) {
        for (int i = 0; i < (int)lines->len; i++) {
            const char *t = lines->pdata[i];
            while (*t == ' ' || *t == '\t') {
                t++;
            }
            if (*t == '[') {
                if (sec_start >= 0) {
                    sec_end = i;
                    break;
                }
                if (!strncmp(t, hdr, strlen(hdr))) {
                    sec_start = i + 1;
                }
            }
        }
    }
    if (ini && sec_start < 0) {
        if (lines->len && *(char *)lines->pdata[lines->len - 1]) {
            g_ptr_array_add(lines, g_strdup(""));
        }
        g_ptr_array_add(lines, g_strdup(hdr));
        g_ptr_array_add(lines, newline);
        return;
    }
    int last = -1, replaced = -1;
    for (int i = sec_start; i < sec_end; i++) {
        const char *val;
        char *k = line_key(lines->pdata[i], ini, &val);
        if (k && !strcmp(k, key)) {
            replaced = i;
        }
        if (k || *(char *)lines->pdata[i]) {
            last = i;
        }
        g_free(k);
    }
    if (replaced >= 0) {
        g_free(lines->pdata[replaced]);
        lines->pdata[replaced] = newline;
        return;
    }
    int at = last >= 0 ? last + 1 : sec_start;
    if (!ini && at > (int)lines->len) {
        at = (int)lines->len;
    }
    g_ptr_array_add(lines, NULL);
    memmove(&lines->pdata[at + 1], &lines->pdata[at], (lines->len - 1 - (guint)at) * sizeof(gpointer));
    lines->pdata[at] = newline;
}

static GPtrArray *text_lines(const char *path)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    char *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **parts = g_strsplit(text, "\n", -1);
        int n = 0;
        while (parts[n]) {
            n++;
        }
        if (n > 0 && !*parts[n - 1]) {
            n--; /* the file's trailing newline is not a line */
        }
        for (int i = 0; i < n; i++) {
            g_ptr_array_add(a, g_strdup(parts[i]));
        }
        g_strfreev(parts);
        g_free(text);
    }
    return a;
}

/* Replaces the first element of SLiM's comma list `sessions`. */
static char *slim_sessions_value(const char *current_list, const char *first)
{
    GString *out = g_string_new(first);
    char **items = g_strsplit(current_list, ",", -1);
    for (char **it = items; *it; it++) {
        char *s = g_strstrip(*it);
        if (*s && strcmp(s, first) != 0) {
            g_string_append_printf(out, ",%s", s);
        }
    }
    g_strfreev(items);
    return g_string_free(out, FALSE);
}

static char *build_new_text(Dm dm, const Cfg *c)
{
    char *target = write_target(dm);
    GPtrArray *l = text_lines(target);
    g_free(target);
    int ini = dm != DM_SLIM;
    const char *user = c->autologin ? c->user : "";
    switch (dm) {
    case DM_SDDM:
        text_set(l, 1, "Autologin", "User", user);
        text_set(l, 1, "Autologin", "Session", c->session);
        if (c->numlock[0]) {
            text_set(l, 1, "General", "Numlock", c->numlock);
        }
        if (c->theme[0]) {
            text_set(l, 1, "Theme", "Current", c->theme);
        }
        break;
    case DM_LIGHTDM:
        text_set(l, 1, "Seat:*", "autologin-user", user);
        if (c->session[0]) {
            text_set(l, 1, "Seat:*", "user-session", c->session);
            text_set(l, 1, "Seat:*", "autologin-session", c->session);
        }
        if (c->theme[0]) {
            text_set(l, 1, "Seat:*", "greeter-session", c->theme);
        }
        break;
    default: {
        text_set(l, 0, "", "auto_login", c->autologin ? "yes" : "no");
        if (c->user[0]) {
            text_set(l, 0, "", "default_user", c->user);
        }
        if (c->session[0]) {
            char cur[512] = "";
            chain_get(dm, "", "sessions", cur, sizeof(cur));
            char *val = slim_sessions_value(cur, c->session);
            text_set(l, 0, "", "sessions", val);
            g_free(val);
        }
        if (c->numlock[0]) {
            text_set(l, 0, "", "numlock", c->numlock);
        }
        if (c->theme[0]) {
            text_set(l, 0, "", "current_theme", c->theme);
        }
        break;
    }
    }
    (void)ini;
    GString *out = g_string_new("");
    for (guint i = 0; i < l->len; i++) {
        g_string_append(out, l->pdata[i]);
        g_string_append_c(out, '\n');
    }
    g_ptr_array_free(l, TRUE);
    return g_string_free(out, FALSE);
}

/* ---- the form ---------------------------------------------------------------- */

static int safe_value(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == '\\' || *s == '`') {
            return 0;
        }
    }
    return 1;
}

static void combo_entry_set(GtkWidget *combo, const char *text)
{
    gtk_entry_set_text(GTK_ENTRY(GTK_BIN(combo)->child), text);
}

static const char *combo_entry_get(GtkWidget *combo)
{
    return gtk_entry_get_text(GTK_ENTRY(GTK_BIN(combo)->child));
}

static void combo_clear(GtkWidget *combo)
{
    GtkTreeModel *m = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    while (gtk_tree_model_iter_n_children(m, NULL) > 0) {
        gtk_combo_box_remove_text(GTK_COMBO_BOX(combo), 0);
    }
}

static void fill_users(void)
{
    combo_clear(g_user_combo);
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent())) {
        if (pw->pw_uid >= 1000 && pw->pw_uid < 60000) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_user_combo), pw->pw_name);
        }
    }
    endpwent();
}

/* Session choices for `dm`, from the xsessions .desktop files. */
static void fill_sessions(Dm dm)
{
    combo_clear(g_session_combo);
    DIR *d = opendir("/usr/share/xsessions");
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= 8 || strcmp(e->d_name + n - 8, ".desktop") != 0) {
            continue;
        }
        char entry[96];
        if (dm == DM_SLIM) {
            char path[PATH_MAX], exec[256];
            if (!fmt_fits(path, sizeof(path), "/usr/share/xsessions/%s", e->d_name) ||
                !desktop_entry_get(path, "Exec", exec, sizeof(exec))) {
                continue;
            }
            char *sp = strchr(exec, ' ');
            if (sp) {
                *sp = '\0';
            }
            snprintf(entry, sizeof(entry), "%.95s", exec);
        } else if (dm == DM_SDDM) {
            snprintf(entry, sizeof(entry), "%s", e->d_name);
        } else {
            snprintf(entry, sizeof(entry), "%.*s", (int)(n - 8), e->d_name);
        }
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_session_combo), entry);
    }
    closedir(d);
}

static void fill_themes(Dm dm)
{
    combo_clear(g_theme_combo);
    const char *dir = dm == DM_SDDM ? "/usr/share/sddm/themes" : dm == DM_SLIM ? "/usr/share/slim/themes" : NULL;
    if (dm == DM_LIGHTDM) {
        DIR *d = opendir("/usr/share/xgreeters");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > 8 && !strcmp(e->d_name + n - 8, ".desktop")) {
                char name[96];
                snprintf(name, sizeof(name), "%.*s", (int)(n - 8), e->d_name);
                gtk_combo_box_append_text(GTK_COMBO_BOX(g_theme_combo), name);
            }
        }
        if (d) {
            closedir(d);
        }
        return;
    }
    DIR *d = dir ? opendir(dir) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] != '.') {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_theme_combo), e->d_name);
        }
    }
    if (d) {
        closedir(d);
    }
}

static void show_dm(Dm dm)
{
    g_dm = dm;
    Cfg c;
    read_cfg(dm, &c);
    fill_sessions(dm);
    fill_themes(dm);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_auto_chk), c.autologin);
    combo_entry_set(g_user_combo, c.user);
    combo_entry_set(g_session_combo, c.session);
    combo_entry_set(g_theme_combo, c.theme);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_numlock_combo), !strcmp(c.numlock, "on") ? 1 : !strcmp(c.numlock, "off") ? 2 : 0);
    gtk_widget_set_sensitive(g_numlock_combo, dm != DM_LIGHTDM);

    char hint[400];
    char *target = write_target(dm);
    snprintf(hint, sizeof(hint), _("Alteracoes vao para %s e valem no proximo login (o gerenciador nao e reiniciado, para nao encerrar a sessao atual)."), target);
    g_free(target);
    gtk_label_set_text(GTK_LABEL(g_hint), hint);
    gtk_label_set_text(GTK_LABEL(g_status), "");
}

static void on_dm_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(g_dm_combo));
    if (i >= 0 && i < g_ncombo) {
        show_dm(g_combo_dm[i]);
        kiconf_track_applied(g_dm_combo); /* that manager's saved settings */
    }
}

static void on_apply(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    Cfg c;
    memset(&c, 0, sizeof(c));
    c.autologin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_auto_chk));
    snprintf(c.user, sizeof(c.user), "%s", combo_entry_get(g_user_combo));
    snprintf(c.session, sizeof(c.session), "%s", combo_entry_get(g_session_combo));
    snprintf(c.theme, sizeof(c.theme), "%s", combo_entry_get(g_theme_combo));
    int nl = gtk_combo_box_get_active(GTK_COMBO_BOX(g_numlock_combo));
    snprintf(c.numlock, sizeof(c.numlock), "%s", nl == 1 ? "on" : nl == 2 ? "off" : "");

    if (!safe_value(c.user) || !safe_value(c.session) || !safe_value(c.theme)) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Valor invalido: sem caracteres de controle, '\\' ou '`'."));
        return;
    }
    if (c.autologin && (!c.user[0] || !getpwnam(c.user))) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Escolha um usuario existente para o login automatico."));
        return;
    }
    if (c.autologin) {
        Cfg cur;
        read_cfg(g_dm, &cur);
        if (!cur.autologin) {
            GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(g_dm_combo)), GTK_DIALOG_MODAL,
                GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                _("Com o login automatico, qualquer pessoa que ligar este computador entra como %s sem senha. Ativar?"), c.user);
            dialog_add_answers(dlg, GTK_BUTTONS_YES_NO);
            int r = gtk_dialog_run(GTK_DIALOG(dlg));
            gtk_widget_destroy(dlg);
            if (r != GTK_RESPONSE_YES) {
                return;
            }
        }
    }

    char *text = build_new_text(g_dm, &c);
    char *args[] = {(char *)DMS[g_dm].id, (char *)write_mode(g_dm), NULL};
    int rc = run_privileged_script(SCRIPT, args, text);
    g_free(text);
    if (rc == 0) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Aplicado. Vale a partir do proximo login."));
        show_dm(g_dm);
        gtk_label_set_text(GTK_LABEL(g_status), _("Aplicado. Vale a partir do proximo login."));
        kiconf_track_applied(g_status);
    } else if (rc == 126 || rc == 127) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Autorizacao negada ou cancelada."));
    } else {
        gtk_label_set_text(GTK_LABEL(g_status), _("Falha ao gravar a configuracao."));
    }
}

static int dm_installed(Dm dm)
{
    static const char *const dirs[] = {"/usr/sbin", "/usr/bin", "/usr/local/bin", "/sbin", "/bin"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char p[PATH_MAX];
        if (fmt_fits(p, sizeof(p), "%s/%s", dirs[i], DMS[dm].bin) && access(p, X_OK) == 0) {
            return 1;
        }
    }
    return 0;
}

GtkWidget *build_login_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    GtkWidget *info = gtk_label_new(_(
        "Gerenciador de login (SDDM, LightDM ou SLiM). Ver as configuracoes nao exige senha; "
        "gravar pede a senha de administrador."));
    gtk_misc_set_alignment(GTK_MISC(info), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), info, FALSE, FALSE, 0);

    /* The active manager, from the Debian alternatives file, picks the
     * initial choice; every installed one can still be configured. */
    char *dm_path = NULL;
    Dm active = N_DM;
    if (g_file_get_contents("/etc/X11/default-display-manager", &dm_path, NULL, NULL)) {
        g_strstrip(dm_path);
        const char *base = strrchr(dm_path, '/');
        base = base ? base + 1 : dm_path;
        for (int i = 0; i < N_DM; i++) {
            if (!strcmp(base, DMS[i].bin)) {
                active = (Dm)i;
            }
        }
        g_free(dm_path);
    }

    GtkWidget *table = gtk_table_new(7, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 8);

    g_dm_combo = gtk_combo_box_new_text();
    g_ncombo = 0;
    int initial = 0;
    for (int i = 0; i < N_DM; i++) {
        g_dm_avail[i] = dm_installed((Dm)i);
        if (!g_dm_avail[i]) {
            continue;
        }
        char label[64];
        snprintf(label, sizeof(label), "%s%s", DMS[i].label, i == (int)active ? _(" (em uso)") : "");
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_dm_combo), label);
        if (i == (int)active) {
            initial = g_ncombo;
        }
        g_combo_dm[g_ncombo++] = (Dm)i;
    }
    labeled_row(table, 0, _("Gerenciador:"), g_dm_combo);

    g_auto_chk = gtk_check_button_new_with_label(_("Login automatico"));
    gtk_table_attach(GTK_TABLE(table), g_auto_chk, 0, 2, 1, 2, GTK_FILL, GTK_FILL, 0, 0);

    g_user_combo = gtk_combo_box_entry_new_text();
    fill_users();
    labeled_row(table, 2, _("Usuario:"), g_user_combo);

    g_session_combo = gtk_combo_box_entry_new_text();
    labeled_row(table, 3, _("Sessao:"), g_session_combo);

    g_numlock_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_numlock_combo), _("Nao alterar"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_numlock_combo), _("Ligar"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_numlock_combo), _("Desligar"));
    labeled_row(table, 4, _("Num Lock:"), g_numlock_combo);

    g_theme_combo = gtk_combo_box_entry_new_text();
    labeled_row(table, 5, _("Tema / greeter:"), g_theme_combo);
    gtk_box_pack_start(GTK_BOX(outer), table, FALSE, FALSE, 0);

    g_hint = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_hint), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_hint), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), g_hint, FALSE, FALSE, 0);

    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    GtkWidget *apply = gtk_button_new_with_label(_("Aplicar"));
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), NULL);
    gtk_box_pack_end(GTK_BOX(row), apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    g_status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_status), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), g_status, FALSE, FALSE, 0);

    if (g_ncombo == 0) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Nenhum gerenciador de login suportado (SDDM, LightDM, SLiM) foi encontrado."));
        gtk_widget_set_sensitive(table, FALSE);
        gtk_widget_set_sensitive(apply, FALSE);
        return outer;
    }
    g_signal_connect(g_dm_combo, "changed", G_CALLBACK(on_dm_changed), NULL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_dm_combo), initial);
    return outer;
}
