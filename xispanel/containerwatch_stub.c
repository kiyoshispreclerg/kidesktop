/* containerwatch_stub.c - built without libXi: the container popup keeps its
 * pointer grab instead (see xispanel.c's container_grab()). */
#include <X11/Xlib.h>

int cwatch_init(Display *dpy)
{
    (void)dpy;
    return 0;
}

void cwatch_select(Display *dpy, Window root, int on)
{
    (void)dpy;
    (void)root;
    (void)on;
}

int cwatch_is_press(Display *dpy, XEvent *ev)
{
    (void)dpy;
    (void)ev;
    return 0;
}
