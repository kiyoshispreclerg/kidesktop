/* ks_censor.c - windows to black out in screenshots and recordings.
 *
 * A window is censored when it carries the mark _KIWM_CAPTURE_EXCLUDE (set
 * from kiwm's or xispanel's window menu, or by "Hide a Window..." here --
 * kishot keeps no list of its own; the mark is the list, and only the
 * window's menu removes it) or when its WM_CLASS is in censor_classes; with censor_popups its menus and tooltips
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
#include <math.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <string.h>

#define INFLATE 2   /* px around each window, against a frame of lag while it moves */

static KsConfig *cfg;
static GArray *marked;              /* client Windows carrying the mark, main thread only */
static GMutex lock;
static KsRect *rects;
static int n_rects;
static guint idle_id;
static void (*on_changed)(void *);
static void *on_changed_data;

static Display *xdpy(void) { return GDK_DISPLAY_XDISPLAY(gdk_display_get_default()); }

static Atom mark_atom(Display *dpy)
{
    static Atom a;
    if (!a)
        a = XInternAtom(dpy, "_KIWM_CAPTURE_EXCLUDE", False);
    return a;
}

static gboolean has_mark(Display *dpy, Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    gboolean set = FALSE;
    if (XGetWindowProperty(dpy, w, mark_atom(dpy), 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                           &after, &data) == Success) {
        set = type == XA_CARDINAL && fmt == 32 && n > 0;
        if (data)
            XFree(data);
    }
    return set;
}

gboolean ks_censor_marks_only(void)
{
    return !(cfg->censor_classes && cfg->censor_classes[0] && *cfg->censor_classes[0]);
}

gboolean ks_censor_compositor(void)
{
    Display *dpy = xdpy();
    char name[32];
    g_snprintf(name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen(dpy));
    return XGetSelectionOwner(dpy, XInternAtom(dpy, name, False)) != None;
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

typedef struct {
    Window w;
    GdkRectangle r;
    gboolean override;
    gboolean see_through;       /* shaped or ARGB: never hides what is below */
    gboolean censored;
    long pid;
} Top;

/* Returns whether the set of marked windows changed. */
static gboolean recompute(void)
{
    Display *dpy = xdpy();
    KsRect *out = NULL;
    int nout = 0;
    GArray *now = g_array_new(FALSE, FALSE, sizeof(Window));

    {
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
            gboolean mark = FALSE;
            if (client && !t->override) {
                /* the mark can come and go while the window stays: watch it.
                 * Never on our own windows: XSelectInput *replaces* the mask
                 * GDK chose for them, and a window that no longer hears the
                 * pointer and the keyboard is a frozen window. */
                if (!gdk_x11_window_lookup_for_display(gdk_display_get_default(), client))
                    XSelectInput(dpy, client, PropertyChangeMask);
                mark = has_mark(dpy, client);
                if (mark)
                    g_array_append_val(now, client);
            }
            t->censored = mark || class_listed(dpy, client ? client : kids[i]);
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

    gboolean differs = now->len != marked->len ||
        memcmp(now->data, marked->data, now->len * sizeof(Window)) != 0;
    g_array_free(marked, TRUE);
    marked = now;
    return differs;
}

static gboolean recompute_idle(gpointer data)
{
    (void)data;
    idle_id = 0;
    if (recompute() && on_changed)
        on_changed(on_changed_data);
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
        schedule();
        break;
    default:
        break;
    }
    return GDK_FILTER_CONTINUE;
}

/* the mark set or deleted on a client window */
static GdkFilterReturn mark_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    (void)data;
    XEvent *e = xev;
    if (e->type == PropertyNotify && e->xproperty.atom == mark_atom(xdpy()))
        schedule();
    return GDK_FILTER_CONTINUE;
}

void ks_censor_init(KsConfig *c, void (*changed)(void *), void *data)
{
    cfg = c;
    on_changed = changed;
    on_changed_data = data;
    marked = g_array_new(FALSE, FALSE, sizeof(Window));
    gdk_window_add_filter(NULL, mark_filter, NULL);
    GdkWindow *root = gdk_get_default_root_window();
    gdk_window_set_events(root, gdk_window_get_events(root) | GDK_SUBSTRUCTURE_MASK);
    gdk_window_add_filter(root, root_filter, NULL);
    if (recompute() && on_changed)
        on_changed(on_changed_data);
}

static void changed(void)
{
    recompute();
    if (on_changed)
        on_changed(on_changed_data);
}

/* Marks the client inside `w` (what a click on the screen returns: a
 * top-level frame). Marking is all kishot does: it never unmarks. */
void ks_censor_add(unsigned long w)
{
    Display *dpy = xdpy();
    gdk_error_trap_push();
    Window client = find_client(dpy, w, 0);
    if (!client)
        client = w;
    unsigned long one = 1;
    XChangeProperty(dpy, client, mark_atom(dpy), XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&one, 1);
    gdk_flush();
    gdk_error_trap_pop();
    changed();
}

void ks_censor_refresh(void)
{
    recompute();
}

/* the client windows carrying the mark right now */
GArray *ks_censor_marked(void)
{
    return marked;
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

void ks_censor_paint_scaled(GdkPixbuf *pb, const KsRect *origin, double scale)
{
    int n;
    KsRect *rs = ks_censor_rects(&n);
    int W = gdk_pixbuf_get_width(pb), H = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    for (int i = 0; i < n; i++) {
        /* rounded outwards: a scaled-down preview never shows a sliver */
        int x1 = MAX((int)floor((rs[i].x - origin->x) * scale), 0);
        int y1 = MAX((int)floor((rs[i].y - origin->y) * scale), 0);
        int x2 = MIN((int)ceil((rs[i].x + rs[i].w - origin->x) * scale), W);
        int y2 = MIN((int)ceil((rs[i].y + rs[i].h - origin->y) * scale), H);
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

void ks_censor_paint(GdkPixbuf *pb, const KsRect *origin)
{
    ks_censor_paint_scaled(pb, origin, 1.0);
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
