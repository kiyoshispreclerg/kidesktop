/* kiconf - shared helpers used by 2+ tabs. See kiconf.c's top doc
 * comment for the overall design and common.h for what lives here. */
#include "common.h"

#include <gdk/gdkx.h>

#include "../shared/xis_outputs.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- config path resolution (mirrors kiconfd.c/xiskeys.c exactly) ---- */

void resolve_path(const char *filename, char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/%s", xdg_config, filename);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(out, outsz, "%s/.config/%s", home, filename);
}

char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return s;
}

/* Sends SIGHUP to every process named `procname` (exact match, not -f --
 * safe against accidentally hitting unrelated processes). Fire-and-forget,
 * same spirit as run_action() elsewhere in this repo's daemons. */
void signal_daemon(const char *procname)
{
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execlp("pkill", "pkill", "-HUP", "-x", procname, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
}

void spawn_replace(const char *prog)
{
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        setsid();
        execlp(prog, prog, "--replace", (char *)NULL);
        _exit(127);
    }
    /* No waitpid: prog is meant to outlive kiconf by the rest of the
     * session, so there is nothing to usefully wait for. */
}

int process_running(const char *procname)
{
    char *argv[] = {"pgrep", "-x", (char *)procname, NULL};
    char out[64];
    return run_capture(argv, out, sizeof(out)) && out[0];
}

/* ---- generic subprocess helpers (xrandr/xinput/xset/xprop/wmctrl) ---- */

/* Runs argv (NULL-terminated), waits for it, and returns its exit code
 * (-1 if it couldn't even be started). Used for "apply" commands where
 * kiconf doesn't need the output, just whether it worked. */
int run_fire(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Runs argv and captures its stdout into `out` (truncated to outsz-1).
 * Returns 1 on success (process ran and exited 0), 0 otherwise -- `out`
 * is always NUL-terminated either way (possibly empty). */
int run_capture(char *const argv[], char *out, size_t outsz)
{
    out[0] = '\0';
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < outsz && (n = read(pipefd[0], out + total, outsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    out[total] = '\0';
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- generic JSON-line-over-Unix-socket client (xisguard-ctl/xispanel-ctl) */

int json_line_send(const char *sockpath, const char *req, char *resp, size_t respsz)
{
    resp[0] = '\0';
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }
    char line[1024];
    snprintf(line, sizeof(line), "%s\n", req);
    if (write(fd, line, strlen(line)) < 0) {
        close(fd);
        return 0;
    }
    shutdown(fd, SHUT_WR);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < respsz && (n = read(fd, resp + total, respsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(fd);
    return total > 0;
}

void xispanel_reload(void)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/xispanel-ctl.sock", (rundir && *rundir) ? rundir : "/tmp");
    char resp[JSON_BUF_LEN];
    json_line_send(path, "{\"cmd\":\"RELOAD\"}", resp, sizeof(resp));
}

char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

char *next_field(char **cursor)
{
    char *p = skip_ws(*cursor);
    if (!*p) {
        *cursor = p;
        return NULL;
    }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p) {
        *p = '\0';
        p++;
    }
    *cursor = p;
    return start;
}

GtkWidget *labeled_row(GtkWidget *table, int row, const char *label_text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1, GTK_FILL, GTK_FILL, 4, 3);
    gtk_table_attach(GTK_TABLE(table), widget, 1, 2, row, row + 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 4, 3);
    return widget;
}

GtkWidget *frame_with(const char *title, GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

GtkWidget *make_color_button(const char *hex)
{
    GdkColor c;
    if (!gdk_color_parse(hex, &c)) {
        gdk_color_parse("#000000", &c);
    }
    return gtk_color_button_new_with_color(&c);
}

void color_button_hex(GtkWidget *btn, char *out, size_t outsz)
{
    GdkColor c;
    gtk_color_button_get_color(GTK_COLOR_BUTTON(btn), &c);
    snprintf(out, outsz, "#%02x%02x%02x", c.red >> 8, c.green >> 8, c.blue >> 8);
}

static int combo_option_index(const char *const *options, const char *val)
{
    for (int i = 0; options[i]; i++) {
        if (!strcmp(options[i], val)) {
            return i;
        }
    }
    return 0;
}

GtkWidget *make_options_combo(const char *const *options, const char *current)
{
    GtkWidget *combo = gtk_combo_box_new_text();
    for (int i = 0; options[i]; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), options[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), combo_option_index(options, current));
    return combo;
}

const char *combo_text(GtkWidget *combo, const char *const *options)
{
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    return idx >= 0 && options[idx] ? options[idx] : options[0];
}

/* ---- output picker: scans live outputs (shared/xis_outputs.h) so the
 * Wallpaper/Telas tabs' "which monitor" fields are a combobox like every
 * other "pick something that exists" field in this program (theme/icon/
 * cursor -- see kiconf.c's own top doc comment), instead of a free-text
 * field the user would otherwise have to fill with a hand-typed
 * "edid:VVV:PPPP:SSSSSSSS" id. The combo's *value* per row (what
 * output_combo_value() returns) is that id when the output has one, or
 * its bare connector name when it doesn't -- never the display label,
 * which also shows the connector name for a human to recognize the
 * monitor by. */

#define OUTPUT_COMBO_IDS_KEY "xis-output-ids"
#define OUTPUT_COMBO_N_KEY "xis-output-n"

static void output_combo_ids_append(GtkWidget *combo, const char *value)
{
    int n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(combo), OUTPUT_COMBO_N_KEY));
    char *ids = g_object_get_data(G_OBJECT(combo), OUTPUT_COMBO_IDS_KEY);
    ids = g_realloc(ids, (size_t)(n + 1) * XIS_OUTPUT_STR_LEN);
    snprintf(ids + (size_t)n * XIS_OUTPUT_STR_LEN, XIS_OUTPUT_STR_LEN, "%s", value);
    /* g_object_set_data_full() would free the old block on replace, but
     * that old block is exactly what we just g_realloc()'d from (already
     * freed/moved by g_realloc itself) -- steal the slot back with plain
     * set_data first so it doesn't get double-freed. */
    g_object_steal_data(G_OBJECT(combo), OUTPUT_COMBO_IDS_KEY);
    g_object_set_data_full(G_OBJECT(combo), OUTPUT_COMBO_IDS_KEY, ids, g_free);
    g_object_set_data(G_OBJECT(combo), OUTPUT_COMBO_N_KEY, GINT_TO_POINTER(n + 1));
}

/* Selects `current` in `combo` (built by make_output_combo()), appending
 * one more row for it first if it's not among the currently live
 * outputs -- a saved id for a monitor that's unplugged right now, or a
 * plain literal name from a config predating "edid:..." ids, must stay
 * visible/selected rather than silently jumping to something else. */
void output_combo_select(GtkWidget *combo, const char *current)
{
    if (!current || !current[0]) {
        current = "*";
    }
    int n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(combo), OUTPUT_COMBO_N_KEY));
    char *ids = g_object_get_data(G_OBJECT(combo), OUTPUT_COMBO_IDS_KEY);
    for (int i = 0; i < n; i++) {
        if (strcmp(ids + (size_t)i * XIS_OUTPUT_STR_LEN, current) == 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), i);
            return;
        }
    }
    char label[96];
    snprintf(label, sizeof(label), "%s (nao conectado agora)", current);
    gtk_combo_box_append_text(GTK_COMBO_BOX(combo), label);
    output_combo_ids_append(combo, current);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), n);
}

/* `include_wildcard`: whether row 0 is "*" (every output) -- the
 * Wallpaper tab wants it (a layer can span every screen), the Telas tab
 * never does (a saved screen-layout entry is always one specific
 * output). `current` is pre-selected the same way output_combo_select()
 * does it (and may be "*", an "edid:..." id, a literal connector name,
 * or empty/NULL to mean "*"). */
GtkWidget *make_output_combo(int include_wildcard, const char *current)
{
    GtkWidget *combo = gtk_combo_box_new_text();
    g_object_set_data(G_OBJECT(combo), OUTPUT_COMBO_N_KEY, GINT_TO_POINTER(0));

    if (include_wildcard) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "* (todas as telas)");
        output_combo_ids_append(combo, "*");
    }

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XisOutput outs[XIS_MAX_OUTPUTS];
    /* forced=1: an interactive, one-off UI action (opening this combo),
     * not a hot path -- see xis_list_outputs()'s own doc comment on why
     * a cached read can carry a stale EDID indefinitely. */
    int n = xis_list_outputs(dpy, outs, XIS_MAX_OUTPUTS, 1);
    for (int i = 0; i < n; i++) {
        const char *value = outs[i].id[0] ? outs[i].id : outs[i].name;
        char label[96];
        if (outs[i].id[0]) {
            /* outs[i].id is "edid:VVV:PPPP:SSSSSSSS" -- skip the
             * "edid:" prefix in the label, it's implementation detail a
             * user picking a monitor from a list doesn't need to see. */
            snprintf(label, sizeof(label), "%s (%s)", outs[i].name, outs[i].id + 5);
        } else {
            snprintf(label, sizeof(label), "%s (sem EDID)", outs[i].name);
        }
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), label);
        output_combo_ids_append(combo, value);
    }

    output_combo_select(combo, current);
    return combo;
}

/* The stored value (an "edid:..." id, a literal connector name, or "*")
 * behind whichever row of `combo` is currently selected -- never the
 * display label. */
void output_combo_value(GtkWidget *combo, char *out, size_t outsz)
{
    int active = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    int n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(combo), OUTPUT_COMBO_N_KEY));
    char *ids = g_object_get_data(G_OBJECT(combo), OUTPUT_COMBO_IDS_KEY);
    if (active >= 0 && active < n && ids) {
        snprintf(out, outsz, "%s", ids + (size_t)active * XIS_OUTPUT_STR_LEN);
    } else {
        snprintf(out, outsz, "*");
    }
}

void fprintf_double(FILE *f, const char *key, double val, int digits)
{
    char fmt[8];
    snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[64];
    g_ascii_formatd(buf, sizeof(buf), fmt, val);
    fprintf(f, "%s=%s\n", key, buf);
}

/* ---- .desktop files ---------------------------------------------------- */

int desktop_entry_get(const char *path, const char *key, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int in_entry = 0;
    int found = 0;
    size_t keylen = strlen(key);
    char line[1024];
    while (!found && fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (l[0] == '[') {
            in_entry = !strcmp(l, "[Desktop Entry]");
            continue;
        }
        if (!in_entry || l[0] == '#' || !*l) {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *k = trim(l);
        if (strlen(k) == keylen && !strcmp(k, key)) {
            snprintf(out, outsz, "%s", trim(eq + 1));
            found = 1;
        }
    }
    fclose(f);
    return found;
}

void desktop_entry_set_key(const char *path, const char *key, const char *value)
{
    FILE *in = fopen(path, "r");
    if (!in) {
        /* New file: the minimal thing kisession.c's own desktop_get()
         * (and desktop_entry_get() above) needs to see this key. */
        if (!value) {
            return; /* nothing to remove from a file that isn't there */
        }
        FILE *f = fopen(path, "w");
        if (!f) {
            g_warning("kiconf: could not write '%s': %s", path, strerror(errno));
            return;
        }
        fprintf(f, "[Desktop Entry]\n%s=%s\n", key, value);
        fclose(f);
        return;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        fclose(in);
        return;
    }

    int in_entry = 0;
    int written = 0;
    size_t keylen = strlen(key);
    char line[1024];
    while (fgets(line, sizeof(line), in)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", line);
        char *l = trim(buf);

        if (l[0] == '[') {
            if (in_entry && !written && value) {
                fprintf(out, "%s=%s\n", key, value);
                written = 1;
            }
            in_entry = !strcmp(l, "[Desktop Entry]");
            fprintf(out, "%s\n", line);
            continue;
        }

        if (in_entry && *l && *l != '#') {
            char *eq = strchr(l, '=');
            if (eq) {
                *eq = '\0';
                if (strlen(trim(l)) == keylen && !strcmp(trim(l), key)) {
                    if (value) {
                        fprintf(out, "%s=%s\n", key, value);
                        written = 1;
                    } /* else: drop the line -- key removed */
                    continue;
                }
            }
        }
        fprintf(out, "%s\n", line);
    }
    if (in_entry && !written && value) {
        fprintf(out, "%s=%s\n", key, value);
        written = 1;
    }
    if (!written && value) {
        /* No [Desktop Entry] group existed at all (a near-empty or
         * malformed file) -- add one rather than silently doing nothing. */
        fprintf(out, "[Desktop Entry]\n%s=%s\n", key, value);
    }

    fclose(in);
    fclose(out);
    rename(tmp, path);
}

/* ---- kisession.conf ----------------------------------------------------- */

const KisessionServiceDef KISESSION_SERVICES[] = {
    {"dbus", "Barramento de sessao (D-Bus) + ambiente de ativacao", 1},
    {"xisguard", "Permissoes XNOTIFY", 1},
    {"kiconfd", "Daemon de tema/cursor/configuracoes", 1},
    {"xismenu", "Registrador do menu de aplicativos (menu global)", 1},
    {"xisback", "Papel de parede", 1},
    {"xispanel", "Painel/barra de tarefas", 1},
    {"xiskeys", "Atalhos globais de teclado", 1},
    {"audio", "pipewire/pulseaudio (so se nada mais ja tiver iniciado um)", 1},
    {"locker", "Bloqueio de tela (xss-lock + i3lock)", 1},
    {"polkit", "Agente de autenticacao polkit", 1},
    {"wm", "Gerenciador de janelas", 1},
    {"kicomp", "Compositor (opcional)", 1},
    {"autostart", "Entradas de inicio automatico XDG (aplicativos instalados)", 1},
};

static int kisession_service_index(const char *name)
{
    for (int i = 0; i < N_KISESSION_SERVICES; i++) {
        if (!strcmp(KISESSION_SERVICES[i].name, name)) {
            return i;
        }
    }
    return -1;
}

void kisession_load(KisessionConfig *c)
{
    c->wm[0] = '\0';
    for (int i = 0; i < N_KISESSION_SERVICES; i++) {
        c->enabled[i] = KISESSION_SERVICES[i].default_enabled;
    }

    char path[PATH_MAX];
    resolve_path("kisession.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *l = trim(line);
        if (!*l) {
            continue;
        }
        if (!strncmp(l, "SERVICE", 7) && (l[7] == ' ' || l[7] == '\t')) {
            char *p = trim(l + 7);
            char *sp = p;
            while (*sp && *sp != ' ' && *sp != '\t') {
                sp++;
            }
            if (*sp) {
                *sp++ = '\0';
            }
            char *val = trim(sp);
            int idx = kisession_service_index(p);
            if (idx >= 0) {
                c->enabled[idx] = (val[0] == '0') ? 0 : 1;
            }
            continue;
        }
        if (!strncmp(l, "wm", 2)) {
            char *eq = strchr(l, '=');
            if (eq) {
                snprintf(c->wm, sizeof(c->wm), "%s", trim(eq + 1));
            }
        }
    }
    fclose(f);
}

void kisession_save(const KisessionConfig *c)
{
    char path[PATH_MAX];
    resolve_path("kisession.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# kisession config (written by kiconf's Programas padrao / "
                "Iniciar automaticamente tabs)\n");
    fprintf(f, "# See kisession/kisession.c for the full reference.\n\n");
    fprintf(f, "wm = %s\n\n", c->wm);
    for (int i = 0; i < N_KISESSION_SERVICES; i++) {
        fprintf(f, "SERVICE\t%s\t%d\t# %s\n", KISESSION_SERVICES[i].name,
                c->enabled[i], KISESSION_SERVICES[i].doc);
    }
    fclose(f);
    rename(tmp, path);
    signal_daemon("kisession");
}

