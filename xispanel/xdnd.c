/* xdnd.c -- the panel as an XDND drop target that never takes a drop.
 *
 * While another application is dragging something, the drag source holds
 * the pointer grab: the panel gets no MotionNotify/EnterNotify at all, so
 * neither hover nor autohide nor clicks work. The only thing that reaches
 * it is the XDND protocol itself -- if the window under the pointer
 * advertises XdndAware, the source sends it XdndEnter/XdndPosition/
 * XdndLeave. That is all this uses it for: every XdndPosition is answered
 * with a refusing XdndStatus (so the drop never lands on the panel), but
 * it tells us where the drag is. Held still over something a widget's
 * dnd_hover_window() names -- a tasklist button -- for
 * XDND_HOVER_ACTIVATE_MS, that window is activated, so the user can carry
 * the drag on into it. Same thing Plasma's and Windows' taskbars do.
 *
 * Only one drag can be in progress at a time on a display, so a single
 * static state is enough. */

#include "xispanel.h"

#include <X11/Xatom.h>
#include <string.h>

#define XDND_VERSION 5
#define XDND_HOVER_ACTIVATE_MS 1000

static Atom a_aware, a_enter, a_position, a_status, a_leave, a_drop, a_finished;

static struct {
    Window source;      /* None while no drag is over any panel */
    Panel *panel;
    Window target;      /* what the hovered widget would activate */
    uint64_t since_ms;  /* when `target` started being hovered */
    int fired;          /* already activated for this hover */
} s;

void xdnd_init(void)
{
    a_aware = XInternAtom(g_dpy, "XdndAware", False);
    a_enter = XInternAtom(g_dpy, "XdndEnter", False);
    a_position = XInternAtom(g_dpy, "XdndPosition", False);
    a_status = XInternAtom(g_dpy, "XdndStatus", False);
    a_leave = XInternAtom(g_dpy, "XdndLeave", False);
    a_drop = XInternAtom(g_dpy, "XdndDrop", False);
    a_finished = XInternAtom(g_dpy, "XdndFinished", False);
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
    }
    set_target(target, now_ms());

    /* Refused (bit 0 clear), and "keep sending XdndPosition while inside"
     * (bit 1) with an empty rectangle, so every motion reaches us. */
    send_to_source(source, self, a_status, 2, 0, 0, None);
}

int xdnd_handle_event(const XEvent *ev)
{
    if (ev->type != ClientMessage || a_aware == None) {
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

    if (type == a_position) {
        long xy = ev->xclient.data.l[2];
        on_position(p, is_sensor, self, source, (int)((xy >> 16) & 0xffff), (int)(xy & 0xffff));
    } else if (type == a_leave) {
        if (s.source == source) {
            reset();
        }
    } else if (type == a_drop) {
        /* Never accepted, so a conforming source sends XdndLeave instead;
         * answered anyway so a sloppy one doesn't wait out a timeout. */
        send_to_source(source, self, a_finished, 0, None, 0, 0);
        if (s.source == source) {
            reset();
        }
    }
    /* XdndEnter carries nothing we need: the first XdndPosition follows
     * right behind it. */
    return 1;
}

void xdnd_panel_gone(Panel *p)
{
    if (s.panel == p) {
        memset(&s, 0, sizeof(s));
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
