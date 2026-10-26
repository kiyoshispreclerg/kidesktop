/* xprobe - tries one Xnotify-protected action and says whether it worked.
 *
 * Used by matrix.py, which copies this binary to several paths
 * (xprobe-sys, xprobe-user, xprobe-none-N) so that the X server and xisguard
 * see different programs: rules match the executable path.
 *
 *   xprobe SCREEN|SELECTION [--retry-ms N] [--hold-ms N]
 *   XPROBE_ACTION=SCREEN|SELECTION xprobe [--retry-ms N] [--hold-ms N]
 *
 * Prints one line, ALLOW or DENY, and exits 0 (allowed), 1 (denied) or
 * 2 (could not even try). Being killed by xisguard (--always-kill) shows up
 * as death by SIGKILL, which matrix.py reports as KILLED.
 *
 * SCREEN:    maps a window of a known colour and reads a root pixel over it
 *            with GetImage; a denied GetImage returns a zero-filled image.
 * SELECTION: SetSelectionOwner on a private selection, then GetSelectionOwner;
 *            a denied SetSelectionOwner is silently ignored by the server.
 *
 * --retry-ms keeps trying while denied (a prompt answered "allow" grants it
 * a moment later); --hold-ms keeps the connection open after the verdict so
 * xisguard has time to pause or kill us, as it would a real program.
 * XPROBE_RETRY_MS in the environment does the same as --retry-ms without
 * changing the arguments, which "exact" rules (exe|args) match on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define PROBE_PIXEL 0x12AB34UL

static long now_ms(void) {
	struct timespec ts = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(long ms) {
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static int ignore_errors(Display *dpy, XErrorEvent *ev) {
	(void)dpy;
	(void)ev;
	return 0;
}

/* A plain window: there is no window manager to move it, and an
 * override-redirect one would also ask for OVERLAY. */
static Window make_window(Display *dpy) {
	XSetWindowAttributes swa = { 0 };
	swa.background_pixel = PROBE_PIXEL;
	swa.event_mask = ExposureMask | StructureNotifyMask;
	Window w = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, 64, 64, 0,
	                         CopyFromParent, InputOutput, CopyFromParent,
	                         CWBackPixel | CWEventMask, &swa);
	XMapRaised(dpy, w);
	XSync(dpy, False);

	/* Wait (bounded) for the server to have painted the background. */
	long deadline = now_ms() + 1000;
	while (now_ms() < deadline) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (ev.type == Expose)
				return w;
		}
		sleep_ms(10);
	}
	return w;
}

static int try_screen(Display *dpy) {
	XImage *img = XGetImage(dpy, DefaultRootWindow(dpy), 10, 10, 1, 1,
	                        AllPlanes, ZPixmap);
	if (!img)
		return 0;
	unsigned long px = XGetPixel(img, 0, 0) & 0xFFFFFFUL;
	XDestroyImage(img);
	return px == PROBE_PIXEL;
}

static int try_selection(Display *dpy, Window w, Atom sel) {
	XSetSelectionOwner(dpy, sel, w, CurrentTime);
	XSync(dpy, False);
	return XGetSelectionOwner(dpy, sel) == w;
}

int main(int argc, char **argv) {
	/* The action from the environment keeps argv the same for every action,
	 * so one "exact" rule (exe|args) can cover several. */
	int first_opt = 1;
	const char *action = getenv("XPROBE_ACTION");
	if (argc >= 2 && argv[1][0] != '-') {
		action = argv[1];
		first_opt = 2;
	}
	if (!action) {
		fprintf(stderr, "usage: %s SCREEN|SELECTION [--retry-ms N] [--hold-ms N]\n", argv[0]);
		return 2;
	}
	int is_screen = strcmp(action, "SCREEN") == 0;
	if (!is_screen && strcmp(action, "SELECTION") != 0) {
		fprintf(stderr, "xprobe: unknown action %s\n", action);
		return 2;
	}
	const char *env_retry = getenv("XPROBE_RETRY_MS");
	long retry_ms = env_retry ? atol(env_retry) : 0, hold_ms = 0;
	for (int i = first_opt; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--retry-ms") == 0)
			retry_ms = atol(argv[i + 1]);
		else if (strcmp(argv[i], "--hold-ms") == 0)
			hold_ms = atol(argv[i + 1]);
	}

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "xprobe: cannot open display\n");
		return 2;
	}
	XSetErrorHandler(ignore_errors);

	Window w = make_window(dpy);
	/* One selection per process: matrix.py runs several probes at once. */
	char sel_name[64] = { 0 };
	snprintf(sel_name, sizeof(sel_name), "XISGUARD_TEST_SELECTION_%d", (int)getpid());
	Atom sel = XInternAtom(dpy, sel_name, False);

	long deadline = now_ms() + retry_ms;
	int ok;
	for (;;) {
		ok = is_screen ? try_screen(dpy) : try_selection(dpy, w, sel);
		if (ok || now_ms() >= deadline)
			break;
		sleep_ms(100);
	}

	printf("%s\n", ok ? "ALLOW" : "DENY");
	fflush(stdout);

	if (hold_ms > 0) {
		sleep_ms(hold_ms);
		XSync(dpy, False);
	}
	XCloseDisplay(dpy);
	return ok ? 0 : 1;
}
