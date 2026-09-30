/* kistoryd - KiDesktop activity history daemon.
 *
 * Logs what happened on the desktop as plain text, one line per event
 * (ks_log.c): windows, focus periods, virtual desktops and outputs
 * (ks_windows.c), files opened (ks_files.c). No screenshots, no content. One instance per X display.
 * Config: $XDG_CONFIG_HOME/kistory.conf (key=value, SIGHUP reloads);
 * data: $XDG_DATA_HOME/kistory/. */
#include "ks_config.h"
#include "ks_files.h"
#include "ks_log.h"
#include "ks_windows.h"

#include <X11/Xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define KISTORYD_VERSION "0.1.0"
#define PRUNE_INTERVAL_S (6 * 3600)

KsConfig ks_conf;

static volatile sig_atomic_t g_quit, g_reload;
static char g_conf_path[512];

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static void conf_load(void)
{
    if (ks_conf.have_private_re)
        regfree(&ks_conf.private_re);
    memset(&ks_conf, 0, sizeof(ks_conf));
    ks_conf.min_dwell_s = 3;
    ks_conf.retention_days = 90;
    ks_conf.fd_sampling = 1;
    snprintf(ks_conf.exclude, sizeof(ks_conf.exclude), "keepassxc,KeePassXC");
    snprintf(ks_conf.guard_actions, sizeof(ks_conf.guard_actions), "SCREEN,RECORD,INPUT_INJECT,INPUT");

    FILE *f = fopen(g_conf_path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *t = trim(line);
            if (!*t || *t == '#')
                continue;
            char *eq = strchr(t, '=');
            if (!eq)
                continue;
            *eq = '\0';
            char *k = trim(t), *v = trim(eq + 1);
            if (!strcmp(k, "min_dwell_s"))
                ks_conf.min_dwell_s = atoi(v);
            else if (!strcmp(k, "retention_days"))
                ks_conf.retention_days = atoi(v);
            else if (!strcmp(k, "exclude"))
                snprintf(ks_conf.exclude, sizeof(ks_conf.exclude), "%s", v);
            else if (!strcmp(k, "title_only_exclude"))
                snprintf(ks_conf.title_only_exclude, sizeof(ks_conf.title_only_exclude), "%s", v);
            else if (!strcmp(k, "private_title_regex"))
                snprintf(ks_conf.private_title_regex, sizeof(ks_conf.private_title_regex), "%s", v);
            else if (!strcmp(k, "fd_sampling"))
                ks_conf.fd_sampling = atoi(v);
            else if (!strcmp(k, "guard_actions"))
                snprintf(ks_conf.guard_actions, sizeof(ks_conf.guard_actions), "%s", v);
        }
        fclose(f);
    }
    if (ks_conf.private_title_regex[0]) {
        if (regcomp(&ks_conf.private_re, ks_conf.private_title_regex, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0)
            ks_conf.have_private_re = 1;
        else
            fprintf(stderr, "kistoryd: invalid private_title_regex, ignored\n");
    }
}

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_quit = 1;
}

static int on_x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;   /* windows vanish between two requests all the time */
}

static int display_number(void)
{
    const char *d = getenv("DISPLAY");
    const char *colon = d ? strrchr(d, ':') : NULL;
    return colon ? atoi(colon + 1) : 0;
}

static void xdg_dir(char *out, size_t sz, const char *env, const char *fallback, const char *leaf)
{
    const char *base = getenv(env);
    if (base && *base)
        snprintf(out, sz, "%s/%s", base, leaf);
    else
        snprintf(out, sz, "%s/%s/%s", getenv("HOME") ? getenv("HOME") : ".", fallback, leaf);
}

static int acquire_lock(int dispnum)
{
    char path[512];
    const char *run = getenv("XDG_RUNTIME_DIR");
    snprintf(path, sizeof(path), "%s/kistoryd.%d.lock", run && *run ? run : "/tmp", dispnum);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("kistoryd %s\n", KISTORYD_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: %s [--version] [--help]\n"
                   "Desktop activity history daemon for KiDesktop.\n"
                   "Config: $XDG_CONFIG_HOME/kistory.conf  Data: $XDG_DATA_HOME/kistory/\n", argv[0]);
            return 0;
        }
        fprintf(stderr, "kistoryd: unknown option %s\n", argv[i]);
        return 2;
    }

    int dispnum = display_number();
    int lock = acquire_lock(dispnum);
    if (lock < 0) {
        fprintf(stderr, "kistoryd: already running on display :%d\n", dispnum);
        return 1;
    }

    xdg_dir(g_conf_path, sizeof(g_conf_path), "XDG_CONFIG_HOME", ".config", "kistory.conf");
    conf_load();

    char data_dir[512];
    xdg_dir(data_dir, sizeof(data_dir), "XDG_DATA_HOME", ".local/share", "kistory");
    if (!ks_log_init(data_dir, dispnum)) {
        fprintf(stderr, "kistoryd: %s is not writable\n", data_dir);
        return 1;
    }
    ks_log_prune(ks_conf.retention_days);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "kistoryd: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(on_x_error);

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    ks_log_event(time(NULL), "session", "kistoryd", "", -1, "", "start", KISTORYD_VERSION);
    ks_files_init();
    ks_windows_on_focus_end(ks_files_focus_end);
    ks_windows_init(dpy);
    fprintf(stderr, "kistoryd %s: logging to %s/events\n", KISTORYD_VERSION, data_dir);

    time_t next_prune = time(NULL) + PRUNE_INTERVAL_S;
    struct pollfd pfd[2] = {
        { .fd = ConnectionNumber(dpy), .events = POLLIN },
        { .fd = ks_files_fd(), .events = POLLIN },
    };
    while (!g_quit) {
        if (g_reload) {
            g_reload = 0;
            conf_load();
        }
        XFlush(dpy);
        if (!XPending(dpy)) {
            int r = poll(pfd, 2, 60 * 1000);
            if (r < 0 && errno != EINTR)
                break;
            if (r > 0 && (pfd[0].revents & (POLLHUP | POLLERR)))
                break;   /* X server gone */
            if (r > 0 && (pfd[1].revents & POLLIN))
                ks_files_handle();
        }
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            ks_windows_handle_event(&ev);
        }
        if (time(NULL) >= next_prune) {
            ks_log_prune(ks_conf.retention_days);
            next_prune = time(NULL) + PRUNE_INTERVAL_S;
        }
    }

    ks_windows_flush();
    ks_log_event(time(NULL), "session", "kistoryd", "", -1, "", "stop", "");
    XCloseDisplay(dpy);
    close(lock);
    return 0;
}
