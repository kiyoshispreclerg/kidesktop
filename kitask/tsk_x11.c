/* tsk_x11.c - what the X server knows about each process: the pixmap
 * memory its clients hold (XRes, like xrestop) and its windows' titles.
 *
 * One XResQueryClientIds call maps every client to its PID; a window belongs
 * to the client whose resource base it falls under, which covers windows
 * without _NET_WM_PID. Remote clients have no PID and are skipped. */
#include "tsk.h"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XRes.h>
#include <gdk/gdkx.h>
#include <string.h>

typedef struct {
    double mem;
    char *title;
    unsigned long window;
    int windows;
} XProc;

static GHashTable *xprocs;   /* pid -> XProc* */
static gboolean have_xres = TRUE;

static void xproc_free(gpointer d)
{
    XProc *x = d;
    g_free(x->title);
    g_free(x);
}

static XProc *xproc(pid_t pid)
{
    XProc *x = g_hash_table_lookup(xprocs, GINT_TO_POINTER(pid));
    if (!x) {
        x = g_new0(XProc, 1);
        x->mem = -1;
        g_hash_table_insert(xprocs, GINT_TO_POINTER(pid), x);
    }
    return x;
}

static char *window_title(Display *dpy, Window w)
{
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    char *title = NULL;
    if (XGetWindowProperty(dpy, w, net_name, 0, 1024, False, utf8, &type, &fmt, &n, &after, &data) == Success &&
        data && n) {
        title = g_strndup((char *)data, n);
    } else {
        char *name = NULL;
        if (XFetchName(dpy, w, &name) && name) {
            title = g_locale_to_utf8(name, -1, NULL, NULL, NULL);
            if (!title)
                title = g_strdup(name);
            XFree(name);
        }
    }
    if (data)
        XFree(data);
    if (title && !g_utf8_validate(title, -1, NULL)) {
        g_free(title);
        title = NULL;
    }
    return title;
}

static long window_pid(Display *dpy, Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long pid = 0;
    if (XGetWindowProperty(dpy, w, XInternAtom(dpy, "_NET_WM_PID", False), 0, 1, False, XA_CARDINAL, &type,
                           &fmt, &n, &after, &data) == Success && data && n && fmt == 32)
        pid = *(long *)data;
    if (data)
        XFree(data);
    return pid;
}

void tsk_x11_sample(unsigned need)
{
    if (!xprocs)
        xprocs = g_hash_table_new_full(NULL, NULL, NULL, xproc_free);
    g_hash_table_remove_all(xprocs);
    GdkDisplay *gd = gdk_display_get_default();
    if (!gd)
        return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    gdk_error_trap_push();

    /* client resource base -> pid */
    int nclients = 0;
    XResClient *clients = NULL;
    GHashTable *base_pid = g_hash_table_new(NULL, NULL);
    unsigned long mask = 0;
    if (have_xres) {
        int ev, err;
        have_xres = XResQueryExtension(dpy, &ev, &err);
    }
    /* XResQueryClients and ...PixmapBytes return True on success, but
     * XResQueryClientIds returns Success (0) */
    if (have_xres && XResQueryClients(dpy, &nclients, &clients)) {
        XResClientIdSpec spec = {0, XRES_CLIENT_ID_PID_MASK};
        long nids = 0;
        XResClientIdValue *ids = NULL;
        if (XResQueryClientIds(dpy, 1, &spec, &nids, &ids) == Success) {
            for (long i = 0; i < nids; i++) {
                pid_t pid = XResGetClientPid(&ids[i]);
                if (pid > 0)
                    g_hash_table_insert(base_pid, GSIZE_TO_POINTER(ids[i].spec.client), GINT_TO_POINTER(pid));
            }
            XResClientIdsDestroy(nids, ids);
        }
        if (nclients)
            mask = clients[0].resource_mask;
        if (need & TSK_NEED_X11)
            for (int i = 0; i < nclients; i++) {
                gpointer pidp = g_hash_table_lookup(base_pid, GSIZE_TO_POINTER(clients[i].resource_base));
                unsigned long bytes = 0;
                if (!pidp || !XResQueryClientPixmapBytes(dpy, clients[i].resource_base, &bytes))
                    continue;
                XProc *x = xproc(GPOINTER_TO_INT(pidp));
                x->mem = MAX(x->mem, 0) + (double)bytes;
            }
    }

    if (need & TSK_NEED_TITLE) {
        Window root = DefaultRootWindow(dpy);
        Atom type;
        int fmt;
        unsigned long n = 0, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, XInternAtom(dpy, "_NET_CLIENT_LIST", False), 0, 4096, False, XA_WINDOW,
                               &type, &fmt, &n, &after, &data) == Success && data && fmt == 32) {
            Window *wins = (Window *)data;
            for (unsigned long i = 0; i < n; i++) {
                long pid = 0;
                if (mask)
                    pid = GPOINTER_TO_INT(g_hash_table_lookup(base_pid, GSIZE_TO_POINTER(wins[i] & ~mask)));
                if (pid <= 0)
                    pid = window_pid(dpy, wins[i]);
                if (pid <= 0)
                    continue;
                XProc *x = xproc((pid_t)pid);
                if (++x->windows == 1) {
                    x->title = window_title(dpy, wins[i]);
                    x->window = wins[i];
                }
            }
        }
        if (data)
            XFree(data);
    }
    if (clients)
        XFree(clients);
    g_hash_table_destroy(base_pid);
    gdk_flush();
    gdk_error_trap_pop();
}

double tsk_x11_mem(pid_t pid)
{
    XProc *x = xprocs ? g_hash_table_lookup(xprocs, GINT_TO_POINTER(pid)) : NULL;
    return x ? x->mem : -1;
}

const char *tsk_x11_title(pid_t pid, unsigned long *window)
{
    XProc *x = xprocs ? g_hash_table_lookup(xprocs, GINT_TO_POINTER(pid)) : NULL;
    *window = x ? x->window : 0;
    return x && x->windows ? (x->title ? x->title : "") : NULL;
}

int tsk_x11_window_count(pid_t pid)
{
    XProc *x = xprocs ? g_hash_table_lookup(xprocs, GINT_TO_POINTER(pid)) : NULL;
    return x ? x->windows : 0;
}

/* _NET_ACTIVE_WINDOW from a pager (source 2): the WM switches desktops and
 * deiconifies as needed */
void tsk_x11_activate(unsigned long window)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = window;
    e.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2;
    e.xclient.data.l[1] = CurrentTime;
    gdk_error_trap_push();
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    gdk_flush();
    gdk_error_trap_pop();
}
