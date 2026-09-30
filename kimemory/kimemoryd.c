/* kimemoryd - KiDesktop clipboard history daemon.
 *
 * Records every CLIPBOARD copy (km_clip.c) with the window/app/document
 * it came from into a plain-text history (km_store.c). One instance per
 * X display. Config: $XDG_CONFIG_HOME/kimemory.conf (key=value, SIGHUP
 * reloads); data: $XDG_DATA_HOME/kimemory/. */
#include "km_clip.h"
#include "km_ctl.h"
#include "km_store.h"

#include <X11/Xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <unistd.h>

#define KIMEMORYD_VERSION "0.1.0"

static volatile sig_atomic_t g_quit, g_reload;
static char g_conf_path[512];

static struct {
    int max_entries;
    int persist;
    KmClipConfig clip;
} conf;

static void conf_defaults(void)
{
    memset(&conf, 0, sizeof(conf));
    conf.max_entries = 200;
    conf.persist = KM_PERSIST_ALL;
    conf.clip.takeover = KM_TAKEOVER_SIMPLE;
    snprintf(conf.clip.exclude, sizeof(conf.clip.exclude), "keepassxc,KeePassXC");
    snprintf(conf.clip.never_takeover, sizeof(conf.clip.never_takeover),
             "libreoffice,soffice,Gimp,gimp,krita,inkscape,Inkscape,blender,Blender");
    conf.clip.max_text = 1024 * 1024;
    conf.clip.max_image = 20 * 1024 * 1024;
    conf.clip.max_mirror = 4 * 1024 * 1024;
}

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
    conf_defaults();
    FILE *f = fopen(g_conf_path, "r");
    if (!f)
        return;
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
        if (!strcmp(k, "max_entries"))
            conf.max_entries = atoi(v);
        else if (!strcmp(k, "persist"))
            conf.persist = !strcasecmp(v, "none") ? KM_PERSIST_NONE
                         : !strcasecmp(v, "favorites") ? KM_PERSIST_FAVORITES : KM_PERSIST_ALL;
        else if (!strcmp(k, "takeover"))
            conf.clip.takeover = !strcasecmp(v, "never") ? KM_TAKEOVER_NEVER
                               : !strcasecmp(v, "onexit") ? KM_TAKEOVER_ONEXIT : KM_TAKEOVER_SIMPLE;
        else if (!strcmp(k, "exclude"))
            snprintf(conf.clip.exclude, sizeof(conf.clip.exclude), "%s", v);
        else if (!strcmp(k, "never_takeover"))
            snprintf(conf.clip.never_takeover, sizeof(conf.clip.never_takeover), "%s", v);
        else if (!strcmp(k, "max_text_kb"))
            conf.clip.max_text = (size_t)atol(v) * 1024;
        else if (!strcmp(k, "max_image_mb"))
            conf.clip.max_image = (size_t)atol(v) * 1024 * 1024;
        else if (!strcmp(k, "max_mirror_mb"))
            conf.clip.max_mirror = (size_t)atol(v) * 1024 * 1024;
    }
    fclose(f);
}

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_quit = 1;
}

/* Windows vanish mid-query all the time (requestors, owners): never fatal. */
static int on_x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
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
    snprintf(path, sizeof(path), "%s/kimemoryd.%d.lock", run && *run ? run : "/tmp", dispnum);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

static void usage(const char *argv0)
{
    printf("Usage: %s [--version] [--help]\n"
           "Clipboard history daemon for KiDesktop.\n"
           "Config: $XDG_CONFIG_HOME/kimemory.conf  Data: $XDG_DATA_HOME/kimemory/\n", argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("kimemoryd %s\n", KIMEMORYD_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        }
        fprintf(stderr, "kimemoryd: unknown option %s\n", argv[i]);
        return 2;
    }

    int dispnum = display_number();
    int lock = acquire_lock(dispnum);
    if (lock < 0) {
        fprintf(stderr, "kimemoryd: already running on display :%d\n", dispnum);
        return 1;
    }

    xdg_dir(g_conf_path, sizeof(g_conf_path), "XDG_CONFIG_HOME", ".config", "kimemory.conf");
    conf_load();

    char data_dir[512];
    xdg_dir(data_dir, sizeof(data_dir), "XDG_DATA_HOME", ".local/share", "kimemory");
    if (!km_store_init(data_dir, conf.max_entries, conf.persist))
        fprintf(stderr, "kimemoryd: warning: %s not writable, history won't be saved\n", data_dir);
    km_store_load();

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "kimemoryd: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(on_x_error);
    if (!km_clip_init(dpy, &conf.clip)) {
        fprintf(stderr, "kimemoryd: XFixes extension missing\n");
        return 1;
    }

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    char ctl_path[256];
    const char *run = getenv("XDG_RUNTIME_DIR");
    snprintf(ctl_path, sizeof(ctl_path), "%s/kimemory-ctl.%d.sock", run && *run ? run : "/tmp", dispnum);
    if (!km_ctl_init(dpy, ctl_path, KIMEMORYD_VERSION))
        fprintf(stderr, "kimemoryd: warning: no control socket at %s\n", ctl_path);

    fprintf(stderr, "kimemoryd %s: %d items in %s\n", KIMEMORYD_VERSION, km_store_count(), data_dir);

    struct pollfd pfd[2] = {
        { .fd = ConnectionNumber(dpy), .events = POLLIN },
        { .fd = km_ctl_fd(), .events = POLLIN },
    };
    while (!g_quit) {
        if (g_reload) {
            g_reload = 0;
            conf_load();
            km_store_set_limits(conf.max_entries, conf.persist);
            km_clip_set_config(&conf.clip);
            km_store_save();
        }
        XFlush(dpy);
        if (!XPending(dpy)) {
            int r = poll(pfd, pfd[1].fd >= 0 ? 2 : 1, km_clip_timeout_ms());
            if (r < 0 && errno != EINTR)
                break;
            if (r > 0 && (pfd[0].revents & (POLLHUP | POLLERR)))
                break;   /* X server gone */
            if (r > 0 && (pfd[1].revents & POLLIN))
                km_ctl_accept();
        }
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            km_clip_handle_event(&ev);
        }
        km_clip_tick();
    }

    km_ctl_close();
    km_store_save();
    XCloseDisplay(dpy);
    close(lock);
    return 0;
}
