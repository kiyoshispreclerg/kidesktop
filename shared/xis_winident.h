/* xis_winident - "which window/app/document is this?", shared by kistoryd
 * and kimemoryd.
 *
 * Both need the same answer for a window at one instant: the managed
 * toplevel, its process, class, title, virtual desktop and output, plus a
 * guess at the document it shows. kimemoryd also gets handed windows that
 * are *not* toplevels -- a selection owner or a paste requestor is usually
 * an unmapped helper window of the client (GTK and Qt both do this) -- so
 * xis_winident_toplevel_for() maps any window back to the managed toplevel
 * of the same X connection.
 *
 * Plain Xlib + XRes + XRandR; no state kept between calls except interned
 * atoms, so every answer is fresh. A window can vanish between two calls:
 * the caller must have an X error handler installed that ignores
 * BadWindow/BadDrawable (Xlib's default one exits the process). */
#ifndef XIS_WINIDENT_H
#define XIS_WINIDENT_H

#include <X11/Xlib.h>
#include <sys/types.h>

typedef struct {
    Window win;             /* the managed toplevel this describes */
    pid_t  pid;             /* _NET_WM_PID, else XRes; 0 if unknown */
    char   wm_class[64];    /* WM_CLASS res_class ("firefox", "Gimp-2.10") */
    char   wm_instance[64]; /* WM_CLASS res_name */
    char   exe[256];        /* /proc/PID/exe, "" if unreadable */
    char   title[512];      /* _NET_WM_NAME, else WM_NAME */
    long   desktop;         /* _NET_WM_DESKTOP; -1 if unset, 0xFFFFFFFF = sticky */
    char   output[64];      /* kiwm's _KIWM_WM_OUTPUT name, else the XRandR monitor under the window's centre; "" if neither */
    char   doc_hint[512];   /* document/page named in the title, see xis_winident_doc_hint(); "" if none */
} XisWinIdent;

/* _NET_ACTIVE_WINDOW, or None. */
Window xis_winident_active(Display *dpy);

/* Fills `out` for `win` (expected to be a managed toplevel; see
 * xis_winident_toplevel_for() otherwise). Returns 1 on success, 0 if the
 * window is gone. */
int xis_winident_get(Display *dpy, Window win, XisWinIdent *out);

/* Maps any window -- toplevel, child, or an unmapped helper window such as
 * a selection owner or a ConvertSelection requestor -- to the managed
 * toplevel (_NET_CLIENT_LIST) it belongs to: itself, an ancestor, or a
 * toplevel created by the same X client connection (the active window
 * wins when that client has several). None if nothing matches. */
Window xis_winident_toplevel_for(Display *dpy, Window w);

/* PID of the X client that created `w`, via XRes; 0 if unknown. */
pid_t xis_winident_client_pid(Display *dpy, Window w);

/* Document guess from a title: the part before the last " - ", " — ",
 * " – " or " | " separator ("banner.xcf-1.0 (RGB…) – GIMP" -> "banner.xcf-1.0
 * (RGB…)", "Page title — Mozilla Firefox" -> "Page title"), with a leading
 * modified marker ("*", "●", "•") stripped. "" when the title has no
 * separator -- a guess is worse than nothing there. */
void xis_winident_doc_hint(const char *title, char *out, size_t outsz);

#endif /* XIS_WINIDENT_H */
