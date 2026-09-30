/* xis_winident - see xis_winident.h. */
#include "xis_winident.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XRes.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    Display *dpy;
    Atom net_active_window, net_client_list, net_wm_pid, net_wm_name,
         net_wm_desktop, utf8_string, kiwm_wm_output, kiwm_outputs;
} A;

static void atoms_init(Display *dpy)
{
    if (A.dpy == dpy)
        return;
    A.dpy = dpy;
    A.net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A.net_client_list   = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A.net_wm_pid        = XInternAtom(dpy, "_NET_WM_PID", False);
    A.net_wm_name       = XInternAtom(dpy, "_NET_WM_NAME", False);
    A.net_wm_desktop    = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    A.utf8_string       = XInternAtom(dpy, "UTF8_STRING", False);
    A.kiwm_wm_output    = XInternAtom(dpy, "_KIWM_WM_OUTPUT", False);
    A.kiwm_outputs      = XInternAtom(dpy, "_KIWM_OUTPUTS", False);
}

/* First CARDINAL/WINDOW item of a 32-bit property. */
static int get_long(Display *dpy, Window w, Atom prop, Atom type, long *out)
{
    Atom at;
    int af;
    unsigned long n, after;
    unsigned char *data = NULL;
    int ok = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, type, &at, &af, &n, &after, &data) == Success &&
        data && n > 0 && af == 32) {
        *out = *(long *)(void *)data;
        ok = 1;
    }
    if (data)
        XFree(data);
    return ok;
}

/* Whole 8-bit property as a string (NULs kept; `len` returns its size). */
static char *get_bytes(Display *dpy, Window w, Atom prop, Atom type, unsigned long *len)
{
    Atom at;
    int af;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, 4096, False, type, &at, &af, &n, &after, &data) != Success ||
        !data || af != 8) {
        if (data)
            XFree(data);
        return NULL;
    }
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, data, n);
        s[n] = '\0';
        if (len)
            *len = n;
    }
    XFree(data);
    return s;
}

static Window *client_list(Display *dpy, unsigned long *count)
{
    Atom at;
    int af;
    unsigned long after;
    unsigned char *data = NULL;
    *count = 0;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), A.net_client_list, 0, 8192, False, XA_WINDOW,
                           &at, &af, count, &after, &data) != Success || !data || af != 32) {
        if (data)
            XFree(data);
        *count = 0;
        return NULL;
    }
    Window *out = malloc(sizeof(Window) * (*count ? *count : 1));
    if (out)
        for (unsigned long i = 0; i < *count; i++)
            out[i] = (Window)((long *)(void *)data)[i];
    XFree(data);
    return out;
}

Window xis_winident_active(Display *dpy)
{
    atoms_init(dpy);
    long v;
    if (get_long(dpy, DefaultRootWindow(dpy), A.net_active_window, XA_WINDOW, &v))
        return (Window)v;
    return None;
}

pid_t xis_winident_client_pid(Display *dpy, Window w)
{
    XResClientIdSpec spec = { .client = w, .mask = XRES_CLIENT_ID_PID_MASK };
    long n = 0;
    XResClientIdValue *ids = NULL;
    pid_t pid = 0;
    if (XResQueryClientIds(dpy, 1, &spec, &n, &ids) == Success) {
        for (long i = 0; i < n && !pid; i++) {
            pid_t p = XResGetClientPid(&ids[i]);
            if (p > 0)
                pid = p;
        }
        XResClientIdsDestroy(n, ids);
    }
    return pid;
}

static void output_name(Display *dpy, Window w, char *out, size_t outsz)
{
    out[0] = '\0';
    Window root = DefaultRootWindow(dpy);

    long idx;
    if (get_long(dpy, w, A.kiwm_wm_output, XA_CARDINAL, &idx) && idx >= 0) {
        unsigned long len = 0;
        char *names = get_bytes(dpy, root, A.kiwm_outputs, A.utf8_string, &len);
        if (names) {
            unsigned long off = 0;
            for (long i = 0; off < len; i++) {
                if (i == idx) {
                    snprintf(out, outsz, "%s", names + off);
                    break;
                }
                off += strlen(names + off) + 1;
            }
            free(names);
        }
        if (out[0])
            return;
    }

    XWindowAttributes wa;
    Window child;
    int x, y;
    if (!XGetWindowAttributes(dpy, w, &wa) || !XTranslateCoordinates(dpy, w, root, 0, 0, &x, &y, &child))
        return;
    int cx = x + wa.width / 2, cy = y + wa.height / 2;
    int n = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &n);
    for (int i = 0; mons && i < n; i++) {
        if (cx >= mons[i].x && cx < mons[i].x + mons[i].width &&
            cy >= mons[i].y && cy < mons[i].y + mons[i].height) {
            char *name = XGetAtomName(dpy, mons[i].name);
            if (name) {
                snprintf(out, outsz, "%s", name);
                XFree(name);
            }
            break;
        }
    }
    if (mons)
        XRRFreeMonitors(mons);
}

void xis_winident_doc_hint(const char *title, char *out, size_t outsz)
{
    static const char *seps[] = { " - ", " \xe2\x80\x94 ", " \xe2\x80\x93 ", " | " };
    out[0] = '\0';
    if (!title || !*title)
        return;

    const char *cut = NULL;
    for (size_t i = 0; i < sizeof(seps) / sizeof(seps[0]); i++) {
        const char *p = title, *last = NULL;
        while ((p = strstr(p, seps[i])) != NULL)
            last = p++;
        if (last && (!cut || last > cut))
            cut = last;
    }
    if (!cut)
        return;

    const char *s = title;
    for (;;) {
        if (*s == '*' || *s == ' ')
            s++;
        else if (!strncmp(s, "\xe2\x97\x8f", 3) || !strncmp(s, "\xe2\x80\xa2", 3)) /* ● • */
            s += 3;
        else
            break;
    }
    if (s >= cut)
        return;
    size_t n = (size_t)(cut - s);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

int xis_winident_get(Display *dpy, Window win, XisWinIdent *out)
{
    atoms_init(dpy);
    memset(out, 0, sizeof(*out));
    out->win = win;
    out->desktop = -1;

    XWindowAttributes wa;
    if (win == None || !XGetWindowAttributes(dpy, win, &wa))
        return 0;

    long v;
    if (get_long(dpy, win, A.net_wm_pid, XA_CARDINAL, &v) && v > 0)
        out->pid = (pid_t)v;
    else
        out->pid = xis_winident_client_pid(dpy, win);

    XClassHint ch;
    if (XGetClassHint(dpy, win, &ch)) {
        snprintf(out->wm_class, sizeof(out->wm_class), "%s", ch.res_class ? ch.res_class : "");
        snprintf(out->wm_instance, sizeof(out->wm_instance), "%s", ch.res_name ? ch.res_name : "");
        if (ch.res_class) XFree(ch.res_class);
        if (ch.res_name) XFree(ch.res_name);
    }

    if (out->pid > 0) {
        char link[64];
        snprintf(link, sizeof(link), "/proc/%d/exe", (int)out->pid);
        ssize_t n = readlink(link, out->exe, sizeof(out->exe) - 1);
        out->exe[n > 0 ? n : 0] = '\0';
    }

    /* Any type: some clients (xdotool) set _NET_WM_NAME as STRING. */
    char *title = get_bytes(dpy, win, A.net_wm_name, AnyPropertyType, NULL);
    if (title && !*title) {
        free(title);
        title = NULL;
    }
    if (!title)
        title = get_bytes(dpy, win, XA_WM_NAME, AnyPropertyType, NULL);
    if (title) {
        snprintf(out->title, sizeof(out->title), "%s", title);
        free(title);
    }

    if (get_long(dpy, win, A.net_wm_desktop, XA_CARDINAL, &v))
        out->desktop = (long)(unsigned long)(unsigned int)v;

    output_name(dpy, win, out->output, sizeof(out->output));
    xis_winident_doc_hint(out->title, out->doc_hint, sizeof(out->doc_hint));
    return 1;
}

static int in_list(const Window *list, unsigned long n, Window w)
{
    for (unsigned long i = 0; i < n; i++)
        if (list[i] == w)
            return 1;
    return 0;
}

Window xis_winident_toplevel_for(Display *dpy, Window w)
{
    atoms_init(dpy);
    if (w == None)
        return None;

    unsigned long n = 0;
    Window *list = client_list(dpy, &n);
    if (!list)
        return None;
    Window found = None;

    /* Itself or an ancestor. */
    for (Window cur = w; cur != None && !found; ) {
        if (in_list(list, n, cur)) {
            found = cur;
            break;
        }
        Window root, parent, *kids = NULL;
        unsigned int nkids;
        if (!XQueryTree(dpy, cur, &root, &parent, &kids, &nkids))
            break;
        if (kids)
            XFree(kids);
        if (parent == root)
            break;
        cur = parent;
    }

    /* Same X client connection: resource ids share the client's base. */
    if (!found) {
        int nc = 0;
        XResClient *clients = NULL;
        if (XResQueryClients(dpy, &nc, &clients) == Success) {
            Window active = xis_winident_active(dpy);
            for (int c = 0; c < nc && !found; c++) {
                XID mask = clients[c].resource_mask;
                if ((w & ~mask) != clients[c].resource_base)
                    continue;
                for (unsigned long i = 0; i < n; i++) {
                    if ((list[i] & ~mask) != clients[c].resource_base)
                        continue;
                    if (list[i] == active || !found)
                        found = list[i];
                }
            }
            if (clients)
                XFree(clients);
        }
    }

    /* Same process, different connection. */
    if (!found) {
        pid_t pid = xis_winident_client_pid(dpy, w);
        Window active = xis_winident_active(dpy);
        for (unsigned long i = 0; pid > 0 && i < n; i++) {
            long p;
            if (get_long(dpy, list[i], A.net_wm_pid, XA_CARDINAL, &p) && p == pid)
                if (list[i] == active || !found)
                    found = list[i];
        }
    }

    free(list);
    return found;
}
