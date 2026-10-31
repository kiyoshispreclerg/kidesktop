/* xis_keycapture.c - see xis_keycapture.h. */
#include "xis_keycapture.h"

#include <gdk/gdkx.h>

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The helper cancels itself after 60 s without a key and dies on its own
 * if its main loop stalls; this is the last resort if both somehow fail,
 * so the keyboard can never stay grabbed indefinitely. */
#define DEADLINE_SECS 300

static struct {
    gboolean running;
    GPid pid;
    int out_fd;
    guint deadline_id;
    XisKeyCaptureDone done;
    gpointer data;
} C = { .out_fd = -1 };

gboolean xis_keycapture_running(void)
{
    return C.running;
}

static gboolean deadline_hit(gpointer d)
{
    (void)d;
    C.deadline_id = 0;
    if (C.running)
        kill(C.pid, SIGKILL);
    return FALSE;
}

static void child_exited(GPid pid, gint status, gpointer d)
{
    (void)d;
    /* The spec is one short line written right before exit: it is all in
     * the pipe by now. */
    char spec[256] = { 0 };
    ssize_t n = C.out_fd >= 0 ? read(C.out_fd, spec, sizeof(spec) - 1) : -1;
    if (n > 0) {
        spec[n] = '\0';
        spec[strcspn(spec, "\r\n")] = '\0';
    } else {
        spec[0] = '\0';
    }
    if (C.out_fd >= 0)
        close(C.out_fd);
    C.out_fd = -1;
    g_spawn_close_pid(pid);
    if (C.deadline_id)
        g_source_remove(C.deadline_id);
    C.deadline_id = 0;
    C.running = FALSE;

    XisKeyCaptureResult r = XIS_KEYCAPTURE_CANCEL;
    if (WIFEXITED(status)) {
        switch (WEXITSTATUS(status)) {
        case 0: r = spec[0] ? XIS_KEYCAPTURE_OK : XIS_KEYCAPTURE_CANCEL; break;
        case 2: r = XIS_KEYCAPTURE_CLEAR; break;
        case 3: r = XIS_KEYCAPTURE_EDIT_TEXT; break;
        default: break;
        }
    }
    if (C.done)
        C.done(r, r == XIS_KEYCAPTURE_OK ? spec : NULL, C.data);
}

gboolean xis_keycapture_start(GtkWidget *parent, const XisKeyCaptureOpts *opts,
                              XisKeyCaptureDone done, gpointer data)
{
    if (C.running)
        return FALSE;

    /* Next to the calling binary first (a build tree, or a PREFIX not on
     * PATH -- both install into the same bindir), then PATH. */
    char xid[32] = { 0 };
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *dir = self ? g_path_get_dirname(self) : NULL;
    char *beside = dir ? g_build_filename(dir, "xiskeycapture", NULL) : NULL;
    g_free(self);
    g_free(dir);
    const char *argv[12];
    int argc = 0;
    argv[argc++] = beside && g_file_test(beside, G_FILE_TEST_IS_EXECUTABLE) ? beside : "xiskeycapture";
    if (opts && opts->title && *opts->title) {
        argv[argc++] = "--title";
        argv[argc++] = opts->title;
    }
    if (opts && opts->current && *opts->current) {
        argv[argc++] = "--current";
        argv[argc++] = opts->current;
    }
    if (opts && opts->modkey && *opts->modkey) {
        argv[argc++] = "--modkey";
        argv[argc++] = opts->modkey;
    }
    if (opts && opts->allow_modifier_only)
        argv[argc++] = "--allow-modifier-only";
    GtkWidget *top = parent ? gtk_widget_get_toplevel(parent) : NULL;
    GdkWindow *win = top ? gtk_widget_get_window(top) : NULL;
    if (win) {
        g_snprintf(xid, sizeof(xid), "0x%lx", (unsigned long)GDK_WINDOW_XID(win));
        argv[argc++] = "--transient-for";
        argv[argc++] = xid;
    }
    argv[argc] = NULL;

    GPid pid;
    int out_fd = -1;
    gboolean ok = g_spawn_async_with_pipes(NULL, (gchar **)argv, NULL,
                                           G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                           NULL, NULL, &pid, NULL, &out_fd, NULL, NULL);
    g_free(beside);
    if (!ok)
        return FALSE;

    C.running = TRUE;
    C.pid = pid;
    C.out_fd = out_fd;
    C.done = done;
    C.data = data;
    g_child_watch_add(pid, child_exited, NULL);
    C.deadline_id = g_timeout_add_seconds(DEADLINE_SECS, deadline_hit, NULL);
    return TRUE;
}
