/* xdnd.c -- the panel as an XDND drop target.
 *
 * While another application is dragging something, the drag source holds
 * the pointer grab: the panel gets no MotionNotify/EnterNotify at all, so
 * neither hover nor autohide nor clicks work. The only thing that reaches
 * it is the XDND protocol itself -- if the window under the pointer
 * advertises XdndAware, the source sends it XdndEnter/XdndPosition/
 * XdndLeave. Two things are done with that:
 *
 *   - Held still over something a widget's dnd_hover_window() names -- a
 *     tasklist button -- for XDND_HOVER_ACTIVATE_MS, that window is
 *     activated, so the user can carry the drag on into it. Same thing
 *     Plasma's and Windows' taskbars do.
 *   - Files (text/uri-list) are accepted wherever a widget's
 *     dnd_accepts_files() says so, and dropped there go to its
 *     dnd_drop_files() -- tasklist opens them with the button's app.
 *     Everywhere else the drop is refused.
 *
 * Only one drag can be in progress at a time on a display, so a single
 * static state is enough. */

#include "xispanel.h"

#include <X11/Xatom.h>
#include <limits.h>
#include <string.h>

#define XDND_VERSION 5
#define XDND_HOVER_ACTIVATE_MS 1000
#define XDND_MAX_FILES 64

static Atom a_aware, a_enter, a_position, a_status, a_leave, a_drop, a_finished;
static Atom a_type_list, a_selection, a_action_copy, a_uri_list, a_data;

static struct {
    Window source;      /* None while no drag is over any panel */
    Panel *panel;
    Window target;      /* what the hovered widget would activate */
    uint64_t since_ms;  /* when `target` started being hovered */
    int fired;          /* already activated for this hover */
    PanelWidget *accept_w; /* widget that would take the drop, or NULL */
    int accept_local_x;
} s;

/* Set by XdndEnter, which comes before any position and is only sent
 * again for the next drag (or the next panel the drag enters). */
static Window offer_source;
static int offer_uris;

/* A drop being fetched: XdndDrop asks for the selection, the data comes
 * back in a SelectionNotify. */
static struct {
    Window self, source;
    PanelWidget *w;
    Panel *panel;
    int local_x;
} pending;

void xdnd_init(void)
{
    a_aware = XInternAtom(g_dpy, "XdndAware", False);
    a_enter = XInternAtom(g_dpy, "XdndEnter", False);
    a_position = XInternAtom(g_dpy, "XdndPosition", False);
    a_status = XInternAtom(g_dpy, "XdndStatus", False);
    a_leave = XInternAtom(g_dpy, "XdndLeave", False);
    a_drop = XInternAtom(g_dpy, "XdndDrop", False);
    a_finished = XInternAtom(g_dpy, "XdndFinished", False);
    a_type_list = XInternAtom(g_dpy, "XdndTypeList", False);
    a_selection = XInternAtom(g_dpy, "XdndSelection", False);
    a_action_copy = XInternAtom(g_dpy, "XdndActionCopy", False);
    a_uri_list = XInternAtom(g_dpy, "text/uri-list", False);
    a_data = XInternAtom(g_dpy, "_XISPANEL_XDND_DATA", False);
}

void xdnd_set_aware(Window win)
{
    if (win == None || a_aware == None) {
        return;
    }
    long version = XDND_VERSION;
    XChangeProperty(g_dpy, win, a_aware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
}

static void send_to_source(Window source, Window self, Atom type, long l1, long l2, long l3, long l4)
{
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = source;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)self;
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    ev.xclient.data.l[3] = l3;
    ev.xclient.data.l[4] = l4;
    XSendEvent(g_dpy, source, False, NoEventMask, &ev);
}

static void reset(void)
{
    if (s.panel) {
        panel_dnd_leave(s.panel);
    }
    memset(&s, 0, sizeof(s));
}

static void set_target(Window target, uint64_t now)
{
    if (target != s.target) {
        s.target = target;
        s.since_ms = now;
        s.fired = 0;
    }
}

static void on_enter(const XClientMessageEvent *cm)
{
    offer_source = (Window)cm->data.l[0];
    offer_uris = 0;
    if (cm->data.l[1] & 1) {
        /* More than three types: the full list is on the source. */
        Atom type;
        int format;
        unsigned long n = 0, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(g_dpy, offer_source, a_type_list, 0, 1024, False, XA_ATOM, &type, &format, &n,
                               &after, &data) == Success && data) {
            for (unsigned long i = 0; i < n; i++) {
                offer_uris |= ((Atom *)data)[i] == a_uri_list;
            }
        }
        if (data) {
            XFree(data);
        }
    } else {
        for (int i = 2; i < 5; i++) {
            offer_uris |= (Atom)cm->data.l[i] == a_uri_list;
        }
    }
}

static void on_position(Panel *p, int is_sensor, Window self, Window source, int root_x, int root_y)
{
    if (s.panel && s.panel != p) {
        reset();
    }
    if (!s.panel) {
        panel_dnd_enter(p);
    }
    s.source = source;
    s.panel = p;
    s.accept_w = NULL;

    Window target = None;
    if (!is_sensor) {
        int x = 0, y = 0;
        Window child;
        XTranslateCoordinates(g_dpy, g_root, p->win, root_x, root_y, &x, &y, &child);
        int horiz = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM);
        int axis_pos = horiz ? x : y, cross_pos = horiz ? y : x;
        panel_dnd_hover(p, axis_pos, cross_pos);
        PanelWidget *w = panel_widget_at(p, axis_pos, cross_pos);
        if (w && w->ops->dnd_hover_window) {
            target = w->ops->dnd_hover_window(w, axis_pos - w->x);
        }
        if (w && offer_uris && offer_source == source && w->ops->dnd_accepts_files &&
            w->ops->dnd_drop_files && w->ops->dnd_accepts_files(w, axis_pos - w->x)) {
            s.accept_w = w;
            s.accept_local_x = axis_pos - w->x;
        }
    }
    set_target(target, now_ms());

    /* Bit 0: accepted or not. Bit 1 with an empty rectangle: "keep
     * sending XdndPosition while inside", so every motion reaches us. */
    if (s.accept_w) {
        send_to_source(source, self, a_status, 3, 0, 0, (long)a_action_copy);
    } else {
        send_to_source(source, self, a_status, 2, 0, 0, None);
    }
}

static void on_drop(Window self, Window source, Time time)
{
    if (s.source != source || !s.accept_w) {
        /* Refused, so a conforming source sends XdndLeave instead;
         * answered anyway so a sloppy one doesn't wait out a timeout. */
        send_to_source(source, self, a_finished, 0, None, 0, 0);
        if (s.source == source) {
            reset();
        }
        return;
    }
    pending.self = self;
    pending.source = source;
    pending.w = s.accept_w;
    pending.panel = s.panel;
    pending.local_x = s.accept_local_x;
    XConvertSelection(g_dpy, a_selection, a_uri_list, a_data, self, time);
    reset();
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One text/uri-list line to a local path: file:// URIs only (with or
 * without a host part), percent-decoded. 0 for anything else. */
static int uri_to_path(const char *uri, size_t len, char *out, size_t outsz)
{
    if (len < 7 || strncmp(uri, "file://", 7) != 0) {
        return 0;
    }
    const char *p = uri + 7, *end = uri + len;
    while (p < end && *p != '/') {
        p++; /* host */
    }
    size_t o = 0;
    for (; p < end && o + 1 < outsz; p++) {
        int hi, lo;
        if (*p == '%' && p + 2 < end && (hi = hex_val(p[1])) >= 0 && (lo = hex_val(p[2])) >= 0) {
            out[o++] = (char)(hi * 16 + lo);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
    return o > 0;
}

static void on_selection_notify(const XSelectionEvent *se)
{
    int ok = 0;
    if (se->property != None) {
        Atom type;
        int format;
        unsigned long n = 0, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(g_dpy, pending.self, a_data, 0, 1 << 18, True, AnyPropertyType, &type, &format, &n,
                               &after, &data) == Success && data && format == 8) {
            static char paths[XDND_MAX_FILES][PATH_MAX];
            const char *argv[XDND_MAX_FILES];
            int count = 0;
            const char *line = (const char *)data, *end = line + n;
            while (line < end && count < XDND_MAX_FILES) {
                const char *eol = memchr(line, '\n', (size_t)(end - line));
                const char *stop = eol ? eol : end;
                size_t len = (size_t)(stop - line);
                if (len > 0 && line[len - 1] == '\r') {
                    len--;
                }
                if (len > 0 && line[0] != '#' && uri_to_path(line, len, paths[count], PATH_MAX)) {
                    argv[count] = paths[count];
                    count++;
                }
                line = eol ? eol + 1 : end;
            }
            if (count > 0) {
                ok = pending.w->ops->dnd_drop_files(pending.w, pending.local_x, argv, count);
            }
        }
        if (data) {
            XFree(data);
        }
    }
    send_to_source(pending.source, pending.self, a_finished, ok, ok ? (long)a_action_copy : None, 0, 0);
    XFlush(g_dpy);
    memset(&pending, 0, sizeof(pending));
}

int xdnd_handle_event(const XEvent *ev)
{
    if (a_aware == None) {
        return 0;
    }
    if (ev->type == SelectionNotify) {
        if (pending.self == None || ev->xselection.requestor != pending.self ||
            ev->xselection.selection != a_selection) {
            return 0;
        }
        on_selection_notify(&ev->xselection);
        return 1;
    }
    if (ev->type != ClientMessage) {
        return 0;
    }
    Atom type = ev->xclient.message_type;
    if (type != a_enter && type != a_position && type != a_leave && type != a_drop) {
        return 0;
    }
    int is_sensor = 0;
    Panel *p = panel_find_by_window(ev->xclient.window, &is_sensor);
    if (!p) {
        return 0;
    }
    Window self = ev->xclient.window;
    Window source = (Window)ev->xclient.data.l[0];

    if (type == a_enter) {
        on_enter(&ev->xclient);
    } else if (type == a_position) {
        long xy = ev->xclient.data.l[2];
        on_position(p, is_sensor, self, source, (int)((xy >> 16) & 0xffff), (int)(xy & 0xffff));
    } else if (type == a_leave) {
        if (s.source == source) {
            reset();
        }
    } else if (type == a_drop) {
        on_drop(self, source, (Time)ev->xclient.data.l[2]);
    }
    return 1;
}

void xdnd_panel_gone(Panel *p)
{
    if (s.panel == p) {
        memset(&s, 0, sizeof(s));
    }
    if (pending.panel == p) {
        memset(&pending, 0, sizeof(pending));
    }
}

void xdnd_tick(uint64_t now)
{
    if (s.target == None || s.fired || now < s.since_ms + XDND_HOVER_ACTIVATE_MS) {
        return;
    }
    s.fired = 1;
    ewmh_activate(s.target);
    XFlush(g_dpy);
}

uint64_t xdnd_next_wake_ms(void)
{
    if (s.target == None || s.fired) {
        return 0;
    }
    return s.since_ms + XDND_HOVER_ACTIVATE_MS;
}
