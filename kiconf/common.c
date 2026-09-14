/* kiconf - shared helpers used by 2+ tabs. See kiconf.c's top doc
 * comment for the overall design and common.h for what lives here. */
#include "common.h"

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

void fprintf_double(FILE *f, const char *key, double val, int digits)
{
    char fmt[8];
    snprintf(fmt, sizeof(fmt), "%%.%df", digits);
    char buf[64];
    g_ascii_formatd(buf, sizeof(buf), fmt, val);
    fprintf(f, "%s=%s\n", key, buf);
}

