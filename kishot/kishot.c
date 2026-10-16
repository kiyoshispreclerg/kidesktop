/* kishot - screenshots and screen recording for KiDesktop (GTK2).
 *
 * One instance per display: a later `kishot ...` (a hotkey, usually) hands
 * its command to the running one over a Unix socket and exits, which is
 * how `kishot --record` stops a recording in progress. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static KsApp *the_app;

static void usage(void)
{
    printf(_("kishot %s - screenshots and screen recording\n\n"
             "Usage: kishot [OPTION]...\n"
             "With no area option, the area used last time is captured.\n\n"
             "  -f, --fullscreen     every monitor\n"
             "  -m, --current        the monitor under the pointer\n"
             "  -a, --activewindow   the active window\n"
             "  -r, --region         a rectangle selected with the mouse\n"
             "  -b, --background     save without showing the window\n"
             "  -R, --record         start recording the area, or stop the recording\n"
             "  -h, --help           show this help\n"
             "  -v, --version        show the version\n"), KISHOT_VERSION);
}

/* An abstract socket (Linux): no file to go stale after a crash, and no
 * sun_path length limit to hit with a long runtime directory. */
static char *socket_name(void)
{
    const char *disp = g_getenv("DISPLAY");
    return g_strdup_printf("kishot-%u-%s", (unsigned)getuid(), disp ? disp : ":0");
}

static socklen_t make_addr(struct sockaddr_un *sa, const char *name)
{
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    size_t n = MIN(strlen(name), sizeof(sa->sun_path) - 1);
    memcpy(sa->sun_path + 1, name, n);
    return offsetof(struct sockaddr_un, sun_path) + 1 + n;
}

static void run_line(const char *line)
{
    char kind[16] = "";
    int area = -1, bg = 0;
    if (sscanf(line, "%15s %d %d", kind, &area, &bg) < 1)
        return;
    ks_app_command(the_app, !strcmp(kind, "record") ? KS_CMD_RECORD : KS_CMD_SHOT, area, bg);
}

static gboolean on_client(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)cond;
    (void)data;
    char *line = NULL;
    if (g_io_channel_read_line(ch, &line, NULL, NULL, NULL) == G_IO_STATUS_NORMAL && line)
        run_line(line);
    g_free(line);
    g_io_channel_shutdown(ch, FALSE, NULL);
    return FALSE;
}

static gboolean on_accept(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)cond;
    (void)data;
    int fd = accept(g_io_channel_unix_get_fd(ch), NULL, NULL);
    if (fd >= 0) {
        GIOChannel *c = g_io_channel_unix_new(fd);
        g_io_channel_set_close_on_unref(c, TRUE);
        g_io_add_watch(c, G_IO_IN | G_IO_HUP, on_client, NULL);
        g_io_channel_unref(c);
    }
    return TRUE;
}

/* TRUE when another instance took the command */
static gboolean forward(const char *path, const char *line)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa;
    socklen_t len = make_addr(&sa, path);
    if (fd < 0)
        return FALSE;
    gboolean ok = connect(fd, (struct sockaddr *)&sa, len) == 0 &&
                  write(fd, line, strlen(line)) == (ssize_t)strlen(line);
    close(fd);
    return ok;
}

static void listen_on(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa;
    socklen_t len = make_addr(&sa, path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, len) < 0 || listen(fd, 4) < 0) {
        g_printerr("kishot: cannot listen on @%s; a second kishot will start its own window\n", path);
        if (fd >= 0)
            close(fd);
        return;
    }
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_add_watch(ch, G_IO_IN, on_accept, NULL);
}

int main(int argc, char **argv)
{
    xis_i18n_init("kishot");
    int area = -1;
    gboolean bg = FALSE, record = FALSE;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("kishot %s\n", KISHOT_VERSION);
            return 0;
        }
        if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) area = KS_AREA_FULL;
        else if (!strcmp(a, "-m") || !strcmp(a, "--current")) area = KS_AREA_MONITOR;
        else if (!strcmp(a, "-a") || !strcmp(a, "--activewindow")) area = KS_AREA_WINDOW;
        else if (!strcmp(a, "-r") || !strcmp(a, "--region")) area = KS_AREA_REGION;
        else if (!strcmp(a, "-b") || !strcmp(a, "--background")) bg = TRUE;
        else if (!strcmp(a, "-R") || !strcmp(a, "--record")) record = TRUE;
        else if (a[0] == '-' && a[1] && strspn(a + 1, "fmarbR") == strlen(a + 1)) {
            /* combined short options, e.g. -bR, -rb */
            for (const char *c = a + 1; *c; c++)
                switch (*c) {
                case 'f': area = KS_AREA_FULL; break;
                case 'm': area = KS_AREA_MONITOR; break;
                case 'a': area = KS_AREA_WINDOW; break;
                case 'r': area = KS_AREA_REGION; break;
                case 'b': bg = TRUE; break;
                case 'R': record = TRUE; break;
                }
        } else {
            fprintf(stderr, _("kishot: unknown option '%s' (try --help)\n"), a);
            return 2;
        }
    }

    char *line = g_strdup_printf("%s %d %d\n", record ? "record" : "shot", area, bg);
    char *sock = socket_name();
    if (forward(sock, line))
        return 0;

    gtk_init(&argc, &argv);
    setlocale(LC_NUMERIC, "C");     /* gtk_init() resets LC_ALL */
    listen_on(sock);

    KsConfig cfg;
    ks_config_load(&cfg);
    the_app = ks_app_new(&cfg);
    run_line(line);
    g_free(line);
    gtk_main();
    /* never leave a recording without its trailer */
    if (ks_rec_active()) {
        ks_rec_stop();
        while (ks_rec_active())
            g_main_context_iteration(NULL, TRUE);
    }
    g_free(sock);
    return 0;
}
