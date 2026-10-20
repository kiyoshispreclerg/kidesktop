/* mapmark - maps one window and reports _KIWM_CAPTURE_EXCLUDE *as the client
 * sees it at its own MapNotify* -- which is the moment that proves the mark
 * was set before the window was mapped (kiwm sets it while handling the
 * MapRequest, ahead of its own map).
 *
 *   mapmark INSTANCE CLASS [TRANSIENT_FOR_WINDOW] [SECONDS]
 * Prints "window=0x... mark=N" and then stays up for SECONDS (default 8).
 * MM_X / MM_Y in the environment give it a position of its own.
 * Build: cc -o mapmark mapmark.c -lX11 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d)
        return 2;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 100, 100, 200, 120, 0, 0, 0xffffff);
    XClassHint ch = { argv[1], argv[2] };
    XSetClassHint(d, w, &ch);
    if (argc > 3 && strtoul(argv[3], NULL, 0))
        XSetTransientForHint(d, w, strtoul(argv[3], NULL, 0));
    if (getenv("MM_X") && getenv("MM_Y")) {      /* a position of its own, so kiwm does not centre it */
        XSizeHints h = { .flags = USPosition, .x = atoi(getenv("MM_X")), .y = atoi(getenv("MM_Y")) };
        XSetWMNormalHints(d, w, &h);
        XMoveWindow(d, w, h.x, h.y);
    }
    XSelectInput(d, w, StructureNotifyMask);
    XMapWindow(d, w);
    XEvent e;
    do XNextEvent(d, &e); while (e.type != MapNotify || e.xmap.window != w);

    Atom a = XInternAtom(d, "_KIWM_CAPTURE_EXCLUDE", False), type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    long mark = 0;
    if (XGetWindowProperty(d, w, a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success && data) {
        if (n)
            mark = *(long *)data;
        XFree(data);
    }
    printf("window=0x%lx mark=%ld\n", w, mark);
    fflush(stdout);
    sleep(argc > 4 ? (unsigned)atoi(argv[4]) : 8);
    return 0;
}
