/* An override-redirect window carrying a given _NET_WM_PID: what a menu or
 * tooltip looks like to the capture protocol's popup rule.
 *   override-window PID [seconds]      build: cc -o override-window override-window.c -lX11 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    Display *d = XOpenDisplay(NULL);
    if (!d)
        return 2;
    XSetWindowAttributes a = { .override_redirect = True };
    Window w = XCreateWindow(d, DefaultRootWindow(d), 300, 300, 100, 60, 0, CopyFromParent,
                             InputOutput, CopyFromParent, CWOverrideRedirect, &a);
    long pid = atol(argv[1]);
    XChangeProperty(d, w, XInternAtom(d, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid, 1);
    XMapRaised(d, w);
    XSync(d, False);
    sleep(argc > 2 ? (unsigned)atoi(argv[2]) : 10);
    return 0;
}
