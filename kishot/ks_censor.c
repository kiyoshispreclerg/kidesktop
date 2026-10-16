/* ks_censor.c - windows to black out in screenshots and recordings.
 *
 * A window is censored when the user picked it (by clicking it) or when its
 * WM_CLASS is in censor_classes; with censor_popups its menus and tooltips
 * (override-redirect windows of the same _NET_WM_PID) go too. What gets
 * painted black is only the part of it that is visible: managed windows
 * stacked above it are cut out, so a window lying on top of a censored one
 * still shows. Only opaque rectangles cut: override-redirect windows never
 * do (that is also what keeps a compositor's full-screen overlay from
 * hiding everything), nor do shaped or ARGB windows, which can be seen
 * through.
 *
 * The list is recomputed from SubstructureNotify on the root, coalesced in
 * an idle -- never per frame, since every query is a round trip to X. The
 * recorder thread takes a copy under the mutex. Only the copy is affected:
 * the screen itself is never touched. */
#include "ks.h"
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <string.h>

#define INFLATE 2   /* px around each window, against a frame of lag while it moves */

static KsConfig *cfg;
static GArray *picked;              /* Window */
static GMutex lock;
static KsRect *rects;
static int n_rects;
static guint idle_id;
static void (*on_changed)(void *);
static void *on_changed_data;

static Display *xdpy(void) { return GDK_DISPLAY_XDISPLAY(gdk_display_get_default()); }

static gboolean has_targets(void)
{
    return picked->len > 0 || (cfg->censor_classes && cfg->censor_classes[0] && *cfg->censor_classes[0]);
}

/* the client window inside a WM frame: the first one with WM_STATE */
static Window find_client(Display *dpy, Window w, int depth)
{
    static Atom wm_state;
    if (!wm_state)
        wm_state = XInternAtom(dpy, "WM_STATE", False);
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, wm_state, 0, 0, False, AnyPropertyType, &type, &fmt,
                           &n, &after, &data) == Success) {
        if (data)
            XFree(data);
        if (type != None)
            return w;
    }
    if (depth >= 3)
        return None;
    Window r, p, *kids = NULL;
    unsigned int nk;
    Window found = None;
    if (XQueryTree(dpy, w, &r, &p, &kids, &nk)) {
        for (unsigned int i = 0; i < nk && !found; i++)
            found = find_client(dpy, kids[i], depth + 1);
        if (kids)
            XFree(kids);
    }
    return found;
}

static long window_pid(Display *dpy, Window w)
{
    static Atom pid_atom;
    if (!pid_atom)
        pid_atom = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long pid = 0;
    if (XGetWindowProperty(dpy, w, pid_atom, 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                           &after, &data) == Success && data) {
        if (n == 1)
            pid = *(long *)data;
        XFree(data);
    }
    return pid;
}

static gboolean class_listed(Display *dpy, Window w)
{
    if (!cfg->censor_classes || !cfg->censor_classes[0])
        return FALSE;
    XClassHint ch = { 0 };
    if (!w || !XGetClassHint(dpy, w, &ch))
        return FALSE;
    gboolean hit = FALSE;
    for (char **c = cfg->censor_classes; *c && !hit; c++) {
        if (!**c)
            continue;
        hit = (ch.res_class && !g_ascii_strcasecmp(*c, ch.res_class)) ||
              (ch.res_name && !g_ascii_strcasecmp(*c, ch.res_name));
    }
    if (ch.res_name)
        XFree(ch.res_name);
    if (ch.res_class)
        XFree(ch.res_class);
    return hit;
}

static gboolean is_picked(Window w)
{
    for (guint i = 0; i < picked->len; i++)
        if (g_array_index(picked, Window, i) == w)
            return TRUE;
    return FALSE;
}

typedef struct {
    Window w;
    GdkRectangle r;
    gboolean override;
    gboolean see_through;       /* shaped or ARGB: never hides what is below */
    gboolean censored;
    long pid;
} Top;

static void recompute(void)
{
    Display *dpy = xdpy();
    KsRect *out = NULL;
    int nout = 0;

    if (has_targets()) {
        gdk_error_trap_push();
        Window r, p, *kids = NULL;
        unsigned int nk = 0;
        XQueryTree(dpy, DefaultRootWindow(dpy), &r, &p, &kids, &nk);
        Top *tops = g_new0(Top, nk ? nk : 1);
        int nt = 0;
        GArray *pids = g_array_new(FALSE, FALSE, sizeof(long));
        for (unsigned int i = 0; i < nk; i++) {     /* bottom to top */
            XWindowAttributes a;
            if (!XGetWindowAttributes(dpy, kids[i], &a) || a.map_state != IsViewable ||
                a.class == InputOnly)
                continue;
            Top *t = &tops[nt++];
            t->w = kids[i];
            t->r = (GdkRectangle){ a.x, a.y, a.width + 2 * a.border_width, a.height + 2 * a.border_width };
            t->override = a.override_redirect;
            t->see_through = a.depth == 32;
            int ev, er, bshaped = 0, cshaped, xb, yb, xc, yc;
            unsigned int wb, hb, wc, hc;
            if (!t->see_through && XShapeQueryExtension(dpy, &ev, &er) &&
                XShapeQueryExtents(dpy, kids[i], &bshaped, &xb, &yb, &wb, &hb, &cshaped, &xc, &yc, &wc, &hc))
                t->see_through = bshaped;
            Window client = t->override ? kids[i] : find_client(dpy, kids[i], 0);
            t->censored = is_picked(kids[i]) || class_listed(dpy, client ? client : kids[i]);
            t->pid = client ? window_pid(dpy, client) : 0;
            if (t->censored && t->pid)
                g_array_append_val(pids, t->pid);
        }
        if (cfg->censor_popups)
            for (int i = 0; i < nt; i++)
                if (tops[i].override && !tops[i].censored && tops[i].pid)
                    for (guint k = 0; k < pids->len; k++)
                        if (g_array_index(pids, long, k) == tops[i].pid)
                            tops[i].censored = TRUE;

        GdkRegion *all = gdk_region_new();
        for (int i = 0; i < nt; i++) {
            if (!tops[i].censored)
                continue;
            GdkRectangle g = tops[i].r;
            g.x -= INFLATE;
            g.y -= INFLATE;
            g.width += 2 * INFLATE;
            g.height += 2 * INFLATE;
            GdkRegion *reg = gdk_region_rectangle(&g);
            for (int j = i + 1; j < nt; j++) {
                if (tops[j].override || tops[j].see_through || tops[j].censored)
                    continue;
                GdkRegion *above = gdk_region_rectangle(&tops[j].r);
                gdk_region_subtract(reg, above);
                gdk_region_destroy(above);
            }
            gdk_region_union(all, reg);
            gdk_region_destroy(reg);
        }
        GdkRectangle *gr = NULL;
        gint ng = 0;
        gdk_region_get_rectangles(all, &gr, &ng);
        out = g_new(KsRect, ng ? ng : 1);
        for (int i = 0; i < ng; i++)
            out[i] = (KsRect){ gr[i].x, gr[i].y, gr[i].width, gr[i].height };
        nout = ng;
        g_free(gr);
        gdk_region_destroy(all);
        g_array_free(pids, TRUE);
        g_free(tops);
        if (kids)
            XFree(kids);
        gdk_flush();
        gdk_error_trap_pop();
    }

    g_mutex_lock(&lock);
    g_free(rects);
    rects = out;
    n_rects = nout;
    g_mutex_unlock(&lock);
}

static gboolean recompute_idle(gpointer data)
{
    (void)data;
    idle_id = 0;
    recompute();
    return FALSE;
}

static void schedule(void)
{
    if (!idle_id)
        idle_id = g_idle_add_full(G_PRIORITY_HIGH_IDLE, recompute_idle, NULL, NULL);
}

static GdkFilterReturn root_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    (void)data;
    XEvent *e = xev;
    switch (e->type) {
    case ConfigureNotify: case MapNotify: case UnmapNotify: case DestroyNotify:
    case ReparentNotify: case CirculateNotify:
        if (e->type == DestroyNotify && is_picked(e->xdestroywindow.window)) {
            ks_censor_remove(e->xdestroywindow.window);
            break;
        }
        if (has_targets())
            schedule();
        break;
    default:
        break;
    }
    return GDK_FILTER_CONTINUE;
}

void ks_censor_init(KsConfig *c, void (*changed)(void *), void *data)
{
    cfg = c;
    on_changed = changed;
    on_changed_data = data;
    picked = g_array_new(FALSE, FALSE, sizeof(Window));
    GdkWindow *root = gdk_get_default_root_window();
    gdk_window_set_events(root, gdk_window_get_events(root) | GDK_SUBSTRUCTURE_MASK);
    gdk_window_add_filter(root, root_filter, NULL);
    recompute();
}

static void changed(void)
{
    recompute();
    if (on_changed)
        on_changed(on_changed_data);
}

void ks_censor_add(unsigned long w)
{
    if (!w || is_picked(w))
        return;
    Window win = w;
    g_array_append_val(picked, win);
    changed();
}

void ks_censor_remove(unsigned long w)
{
    for (guint i = 0; i < picked->len; i++)
        if (g_array_index(picked, Window, i) == w) {
            g_array_remove_index(picked, i);
            break;
        }
    changed();
}

void ks_censor_clear(void)
{
    g_array_set_size(picked, 0);
    changed();
}

GArray *ks_censor_picked(void)
{
    return picked;
}

void ks_censor_set_classes(char **classes)
{
    if (classes != cfg->censor_classes) {
        char **copy = g_strdupv(classes);
        g_strfreev(cfg->censor_classes);
        cfg->censor_classes = copy;
    }
    changed();
}

char *ks_censor_title(unsigned long w)
{
    Display *dpy = xdpy();
    char *title = NULL;
    gdk_error_trap_push();
    Window client = find_client(dpy, w, 0);
    if (!client)
        client = w;
    Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, client, net_name, 0, 256, False, utf8, &type, &fmt, &n,
                           &after, &data) == Success && data) {
        if (n)
            title = g_strndup((char *)data, n);
        XFree(data);
    }
    if (!title) {
        char *name = NULL;
        if (XFetchName(dpy, client, &name) && name) {
            title = g_strdup(name);
            XFree(name);
        }
    }
    gdk_flush();
    gdk_error_trap_pop();
    if (!title)
        title = g_strdup_printf("0x%lx", w);
    return title;
}

KsRect *ks_censor_rects(int *n)
{
    g_mutex_lock(&lock);
    KsRect *copy = n_rects ? g_memdup2(rects, n_rects * sizeof(KsRect)) : NULL;
    *n = n_rects;
    g_mutex_unlock(&lock);
    return copy;
}

void ks_censor_paint(GdkPixbuf *pb, const KsRect *origin)
{
    int n;
    KsRect *rs = ks_censor_rects(&n);
    int W = gdk_pixbuf_get_width(pb), H = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    for (int i = 0; i < n; i++) {
        int x1 = MAX(rs[i].x - origin->x, 0), y1 = MAX(rs[i].y - origin->y, 0);
        int x2 = MIN(rs[i].x + rs[i].w - origin->x, W), y2 = MIN(rs[i].y + rs[i].h - origin->y, H);
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                guchar *d = px + y * stride + x * nch;
                d[0] = d[1] = d[2] = 0;
                if (nch == 4)
                    d[3] = 255;
            }
    }
    g_free(rs);
}

/* ---- picking a window by clicking it ------------------------------------- */

typedef struct { GMainLoop *loop; Window result; } Pick;

static GdkFilterReturn pick_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    Pick *pk = data;
    XEvent *e = xev;
    if (e->type == ButtonPress) {
        pk->result = e->xbutton.button == 1 ? e->xbutton.subwindow : None;
        g_main_loop_quit(pk->loop);
        return GDK_FILTER_REMOVE;
    }
    if (e->type == KeyPress) {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape) {
            pk->result = None;
            g_main_loop_quit(pk->loop);
        }
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

unsigned long ks_censor_pick(void)
{
    Display *dpy = xdpy();
    Window root = DefaultRootWindow(dpy);
    Cursor cross = XCreateFontCursor(dpy, XC_crosshair);
    Pick pk = { g_main_loop_new(NULL, FALSE), None };
    if (XGrabPointer(dpy, root, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None,
                     cross, CurrentTime) != GrabSuccess) {
        XFreeCursor(dpy, cross);
        g_main_loop_unref(pk.loop);
        return 0;
    }
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    /* grabbed events are reported on the root window */
    gdk_window_add_filter(NULL, pick_filter, &pk);
    g_main_loop_run(pk.loop);
    gdk_window_remove_filter(NULL, pick_filter, &pk);
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XFreeCursor(dpy, cross);
    XFlush(dpy);
    g_main_loop_unref(pk.loop);
    return pk.result;
}
