/*
 * containerwatch.c - XInput2 raw button presses, for the open container
 * popup's "clicked outside, dismiss". Raw events are delivered to a
 * listener whatever grab is active and without taking one, which is the
 * point: the popup must not hold the pointer, or a xisserve window opened
 * from it would not get the first click (or any hover). See xispanel.c's
 * container_watch_event().
 */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

static int g_opcode = -1;

int cwatch_init(Display *dpy)
{
    int ev, err, major = 2, minor = 0;
    if (!XQueryExtension(dpy, "XInputExtension", &g_opcode, &ev, &err)) {
        return 0;
    }
    if (XIQueryVersion(dpy, &major, &minor) != Success) {
        g_opcode = -1;
        return 0;
    }
    return 1;
}

void cwatch_select(Display *dpy, Window root, int on)
{
    unsigned char mask[XIMaskLen(XI_RawButtonPress)] = {0};
    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask);
    em.mask = mask;
    if (on) {
        XISetMask(mask, XI_RawButtonPress);
    }
    XISelectEvents(dpy, root, &em, 1);
}

/* 1 if `ev` is a raw press of a real button (not a wheel notch). Frees the
 * event's cookie data for every XInput2 event it looks at. */
int cwatch_is_press(Display *dpy, XEvent *ev)
{
    if (ev->type != GenericEvent || ev->xcookie.extension != g_opcode) {
        return 0;
    }
    int press = 0;
    if (XGetEventData(dpy, &ev->xcookie)) {
        if (ev->xcookie.evtype == XI_RawButtonPress) {
            int b = ((XIRawEvent *)ev->xcookie.data)->detail;
            press = b < 4 || b > 7;
        }
        XFreeEventData(dpy, &ev->xcookie);
    }
    return press;
}
