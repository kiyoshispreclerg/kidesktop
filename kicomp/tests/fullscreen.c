/* Sends _NET_WM_STATE_FULLSCREEN (add or remove) for a window to the window
 * manager, the way a toolkit does:  fullscreen WINDOW 1|0
 * Build: cc -o fullscreen fullscreen.c -lX11 */
#include <X11/Xlib.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d)
        return 2;
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = strtoul(argv[1], NULL, 0);
    e.xclient.message_type = XInternAtom(d, "_NET_WM_STATE", False);
    e.xclient.format = 32;
    e.xclient.data.l[0] = atoi(argv[2]);
    e.xclient.data.l[1] = (long)XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    e.xclient.data.l[3] = 1;
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XSync(d, False);
    return 0;
}
