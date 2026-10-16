/*
 * lidpower.c - see lidpower.h.
 *
 * Lid: UPower's LidIsClosed, followed with `gdbus monitor` (no D-Bus
 * library here; the PropertiesChanged lines are trivial to match).
 * Power button: the X server delivers XF86PowerOff, so a root key grab
 * is enough.
 */
#include "lidpower.h"
#include "../shared/xis_spawn.h"

#include <X11/XF86keysym.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MONITOR_RETRY_SEC 30
#define BUTTON_DEBOUNCE_SEC 2

static char g_lid[16] = "system";
static char g_power[16] = "system";

static Display *g_dpy;
static Window g_root;
static KeyCode g_power_keycode;

static pid_t g_monitor_pid;
static int g_monitor_fd = -1;
static time_t g_monitor_started;
static char g_linebuf[1024];
static size_t g_linelen;

static pid_t g_inhibit_pid;
static char g_inhibit_what[64];

static time_t g_last_button;

/* ---- actions --------------------------------------------------------- */

static void run_action(const char *action)
{
    if (!strcmp(action, "suspend")) {
        xis_spawn_detached("systemctl suspend || loginctl suspend || pm-suspend");
    } else if (!strcmp(action, "lock")) {
        xis_spawn_detached("kilock");
    } else if (!strcmp(action, "poweroff")) {
        xis_spawn_detached("systemctl poweroff || loginctl poweroff");
    } else if (!strcmp(action, "ask")) {
        xis_spawn_detached("xisserve --session");
    }
    /* "nothing" and anything unknown: no-op */
}

/* ---- logind inhibitor ------------------------------------------------ */

static void stop_inhibit(void)
{
    if (g_inhibit_pid > 0) {
        killpg(g_inhibit_pid, SIGTERM);
        waitpid(g_inhibit_pid, NULL, 0);
        g_inhibit_pid = 0;
    }
    g_inhibit_what[0] = 0;
}

static void update_inhibit(void)
{
    char what[64] = "";
    if (strcmp(g_lid, "system")) {
        strcat(what, "handle-lid-switch");
    }
    if (strcmp(g_power, "system")) {
        strcat(what, what[0] ? ":handle-power-key" : "handle-power-key");
    }
    if (!strcmp(what, g_inhibit_what) && (g_inhibit_pid > 0) == (what[0] != 0)) {
        return;
    }
    stop_inhibit();
    if (!what[0]) {
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        char arg[96];
        snprintf(arg, sizeof(arg), "--what=%s", what);
        execlp("systemd-inhibit", "systemd-inhibit", arg, "--who=kiconfd", "--why=KiDesktop lid/power button action",
               "--mode=block", "sleep", "infinity", (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid);
    g_inhibit_pid = pid;
    snprintf(g_inhibit_what, sizeof(g_inhibit_what), "%s", what);
}

/* ---- lid monitor ----------------------------------------------------- */

static void stop_monitor(void)
{
    if (g_monitor_pid > 0) {
        kill(g_monitor_pid, SIGTERM);
        waitpid(g_monitor_pid, NULL, 0);
        g_monitor_pid = 0;
    }
    if (g_monitor_fd >= 0) {
        close(g_monitor_fd);
        g_monitor_fd = -1;
    }
    g_linelen = 0;
}

static void start_monitor(void)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return;
    }
    g_monitor_started = time(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDERR_FILENO);
        }
        close(fds[0]);
        close(fds[1]);
        execlp("gdbus", "gdbus", "monitor", "--system", "--dest", "org.freedesktop.UPower", "--object-path",
               "/org/freedesktop/UPower", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    g_monitor_pid = pid;
    g_monitor_fd = fds[0];
    g_linelen = 0;
}

static void handle_line(const char *line)
{
    if (!strstr(line, "'LidIsClosed'")) {
        return;
    }
    if (strstr(line, "'LidIsClosed': <true>")) {
        run_action(g_lid);
    }
}

int lidpower_fd(void)
{
    return g_monitor_fd;
}

void lidpower_on_readable(void)
{
    char buf[512];
    for (;;) {
        ssize_t n = read(g_monitor_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    g_linebuf[g_linelen] = 0;
                    handle_line(g_linebuf);
                    g_linelen = 0;
                } else if (g_linelen + 1 < sizeof(g_linebuf)) {
                    g_linebuf[g_linelen++] = buf[i];
                }
            }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            stop_monitor(); /* UPower gone; lidpower_tick() retries */
        }
        return;
    }
}

void lidpower_tick(void)
{
    if (!strcmp(g_lid, "system") || g_monitor_fd >= 0) {
        return;
    }
    if (time(NULL) - g_monitor_started >= MONITOR_RETRY_SEC) {
        start_monitor();
    }
}

/* ---- power button ---------------------------------------------------- */

static int grab_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

static void update_grab(void)
{
    if (!g_dpy) {
        return;
    }
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(grab_error);
    if (g_power_keycode) {
        XUngrabKey(g_dpy, g_power_keycode, AnyModifier, g_root);
        g_power_keycode = 0;
    }
    if (strcmp(g_power, "system")) {
        KeyCode kc = XKeysymToKeycode(g_dpy, XF86XK_PowerOff);
        if (kc) {
            XGrabKey(g_dpy, kc, AnyModifier, g_root, True, GrabModeAsync, GrabModeAsync);
            g_power_keycode = kc;
        }
    }
    XSync(g_dpy, False);
    XSetErrorHandler(old);
}

void lidpower_handle_key(const XKeyEvent *ev)
{
    if (ev->type != KeyPress || !g_power_keycode || ev->keycode != g_power_keycode) {
        return;
    }
    time_t now = time(NULL);
    if (now - g_last_button < BUTTON_DEBOUNCE_SEC) {
        return;
    }
    g_last_button = now;
    run_action(g_power);
}

/* ---- configuration --------------------------------------------------- */

void lidpower_configure(Display *dpy, Window root, const char *lid_action, const char *power_action)
{
    g_dpy = dpy;
    g_root = root;
    snprintf(g_lid, sizeof(g_lid), "%s", lid_action && *lid_action ? lid_action : "system");
    snprintf(g_power, sizeof(g_power), "%s", power_action && *power_action ? power_action : "system");

    update_inhibit();
    update_grab();
    if (!strcmp(g_lid, "system")) {
        stop_monitor();
    } else if (g_monitor_fd < 0) {
        start_monitor();
    }
}

void lidpower_shutdown(void)
{
    stop_monitor();
    stop_inhibit();
}
