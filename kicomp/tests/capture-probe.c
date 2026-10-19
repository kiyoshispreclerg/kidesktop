/* capture-probe - a minimal recorder for kicomp's capture protocol
 * (CAPTURE-PROTOCOL.md), for testing the compositor's side of it.
 *
 *   capture-probe [--output NAME] [--hide-pid PID] [--request] [--seconds N]
 *
 * Prints one line per thing it learns, flushing each, so a script can wait
 * for them:
 *   OFFER version=1 features=0x1 mode=1 colour=0x000000      (or NO-OFFER)
 *   STATUS state=1 error=0 outputs=1 version=1
 *   TARGET i flags=0x0 pos=0,0 size=800x600 pixels=800x600 depth=24 slots=0
 *   STATE i epoch=1 visible=0 flags=0x0
 *   FRAME output=0 slot=0xffffffff request=7 flags=0x4
 * Exits after N seconds (default 10) or on SIGTERM, sending CLOSE first.
 *
 * Build: cc -o capture-probe capture-probe.c -lX11 -lXrandr */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

static Display *dpy;
static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }

static long *cardinals(Window w, Atom a, unsigned long *n)
{
    Atom type; int fmt; unsigned long after; unsigned char *data = NULL;
    *n = 0;
    if (XGetWindowProperty(dpy, w, a, 0, 4096, False, XA_CARDINAL, &type, &fmt, n,
                           &after, &data) != Success || type != XA_CARDINAL || fmt != 32) {
        if (data) XFree(data);
        *n = 0;
        return NULL;
    }
    return (long *)data;
}

static void print_state(Window s)
{
    unsigned long n;
    long *v = cardinals(s, atom("_KICOMP_CAPTURE_STATE"), &n);
    for (unsigned long i = 0; v && i + 2 < n + 0 && i % 3 == 0; i += 3)
        printf("STATE %lu epoch=%lu visible=%lu flags=0x%lx\n", i / 3,
               (unsigned long)(uint32_t)v[i], (unsigned long)(uint32_t)v[i + 1],
               (unsigned long)(uint32_t)v[i + 2]);
    fflush(stdout);
    if (v) XFree(v);
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    long hide_pid = 0;
    int request = 0, seconds = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "--hide-pid") && i + 1 < argc) hide_pid = atol(argv[++i]);
        else if (!strcmp(argv[i], "--request")) request = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 2; }
    Window root = DefaultRootWindow(dpy);

    char name[64];
    static char first[64];
    if (!output) {
        XRRScreenResources *res = XRRGetScreenResources(dpy, root);
        for (int i = 0; res && i < res->noutput && !first[0]; i++) {
            XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (oi && oi->connection == RR_Connected && oi->crtc)
                snprintf(first, sizeof first, "%s", oi->name);
            if (oi) XRRFreeOutputInfo(oi);
        }
        output = first;
    }

    snprintf(name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen(dpy));
    Window cm = XGetSelectionOwner(dpy, atom(name));
    if (!cm) { printf("NO-COMPOSITOR\n"); return 0; }
    unsigned long n;
    long *offer = cardinals(cm, atom("_KICOMP_CAPTURE"), &n);
    if (!offer || n < 4) { printf("NO-OFFER\n"); return 0; }
    printf("OFFER version=%ld features=0x%lx mode=%ld colour=0x%06lx\n", offer[0],
           (unsigned long)offer[1], offer[2], (unsigned long)offer[3] & 0xffffff);
    XFree(offer);

    Window s = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(dpy, s, PropertyChangeMask);
    char outs[128];
    int ol = snprintf(outs, sizeof outs, "%s", output) + 1;
    XChangeProperty(dpy, s, atom("_KICOMP_CAPTURE_OUTPUTS"), atom("UTF8_STRING"), 8,
                    PropModeReplace, (unsigned char *)outs, ol);
    XChangeProperty(dpy, s, atom("_KICOMP_CAPTURE_CLIENT"), atom("UTF8_STRING"), 8,
                    PropModeReplace, (unsigned char *)"capture-probe", 13);
    long slots = 3;
    XChangeProperty(dpy, s, atom("_KICOMP_CAPTURE_SLOTS"), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&slots, 1);
    if (hide_pid)
        XChangeProperty(dpy, s, atom("_KICOMP_CAPTURE_HIDE_PID"), XA_CARDINAL, 32,
                        PropModeReplace, (unsigned char *)&hide_pid, 1);

    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = cm;
    ev.xclient.message_type = atom("_KICOMP_CAPTURE_OPEN");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = s;
    ev.xclient.data.l[1] = 1;
    XSendEvent(dpy, cm, False, NoEventMask, &ev);
    XFlush(dpy);

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    time_t end = time(NULL) + seconds;
    unsigned req_id = 0;
    int active = 0;
    while (!stop && time(NULL) < end) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (ev.type == ClientMessage) {
                Atom t = ev.xclient.message_type;
                if (t == atom("_KICOMP_CAPTURE_STATUS")) {
                    printf("STATUS state=%ld error=%ld outputs=%ld version=%ld\n",
                           ev.xclient.data.l[0], ev.xclient.data.l[1],
                           ev.xclient.data.l[2], ev.xclient.data.l[3]);
                    active = ev.xclient.data.l[0] == 1;
                    if (active) {
                        unsigned long tn;
                        long *t = cardinals(s, atom("_KICOMP_CAPTURE_TARGETS"), &tn);
                        for (unsigned long i = 0; t && i + 12 < tn + 0; i += 13)
                            printf("TARGET %lu flags=0x%lx pos=%d,%d size=%ux%u pixels=%ux%u depth=%lu slots=%lu\n",
                                   i / 13, (unsigned long)t[i], (int32_t)t[i + 1], (int32_t)t[i + 2],
                                   (uint32_t)t[i + 3], (uint32_t)t[i + 4], (uint32_t)t[i + 5],
                                   (uint32_t)t[i + 6], (unsigned long)t[i + 7], (unsigned long)t[i + 8]);
                        if (t) XFree(t);
                        print_state(s);
                    } else {
                        return 0;
                    }
                } else if (t == atom("_KICOMP_CAPTURE_FRAME")) {
                    printf("FRAME output=%ld slot=0x%lx request=%ld flags=0x%lx\n",
                           ev.xclient.data.l[0], (unsigned long)ev.xclient.data.l[1] & 0xffffffffu,
                           ev.xclient.data.l[2], (unsigned long)ev.xclient.data.l[3]);
                    fflush(stdout);
                }
            } else if (ev.type == PropertyNotify &&
                       ev.xproperty.atom == atom("_KICOMP_CAPTURE_STATE") &&
                       ev.xproperty.state == PropertyNewValue) {
                print_state(s);
                if (request) {
                    XEvent rq;
                    memset(&rq, 0, sizeof rq);
                    rq.xclient.type = ClientMessage;
                    rq.xclient.window = cm;
                    rq.xclient.message_type = atom("_KICOMP_CAPTURE_REQUEST");
                    rq.xclient.format = 32;
                    rq.xclient.data.l[0] = s;
                    rq.xclient.data.l[1] = 0;
                    rq.xclient.data.l[2] = ++req_id;
                    XSendEvent(dpy, cm, False, NoEventMask, &rq);
                    XFlush(dpy);
                }
            }
        }
        struct timeval tv = { 0, 100000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(dpy), &fds);
        select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
    }

    if (active) {
        memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.window = cm;
        ev.xclient.message_type = atom("_KICOMP_CAPTURE_CLOSE");
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = s;
        XSendEvent(dpy, cm, False, NoEventMask, &ev);
        XSync(dpy, False);
        printf("CLOSED\n");
    }
    return 0;
}
