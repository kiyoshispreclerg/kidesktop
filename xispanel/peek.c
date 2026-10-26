/*
 * peek.c - tells the compositor which window's thumbnail is being
 * hovered, so kicomp can fade the other windows out and show that one on
 * its own (effects/peek.c there).
 *
 * The whole protocol is one property on the root window,
 * _KICOMP_PEEK (WINDOW[1]): the client window being pointed at, deleted
 * when the hover ends. It is state, not an event, so it is rewritten about
 * once a second while the hover lasts -- kicomp ends a peek that is not
 * renewed, which is what keeps a crashed panel from leaving the desktop
 * invisible. Without kicomp (or with its peek effect off) nobody reads it
 * and nothing happens.
 *
 * The first peek waits PEEK_DELAY_MS, so sweeping the pointer across a
 * popup does not strobe the desktop; once one is showing, moving to
 * another thumbnail retargets at once.
 */
#include "xispanel.h"

#include <X11/Xatom.h>

#define PEEK_DELAY_MS 400
#define PEEK_RENEW_MS 1000

static Window g_hover = None;     /* what the pointer is on, or None */
static Window g_shown = None;     /* what the property says now */
static uint64_t g_since_ms = 0;   /* hover started (arms the first peek) */
static uint64_t g_renewed_ms = 0; /* last write of the property */
static int g_active = 0;          /* a peek has been published and not ended */

static void publish(Window win, uint64_t now)
{
    static Atom atom = None;
    if (atom == None) {
        atom = XInternAtom(g_dpy, "_KICOMP_PEEK", False);
    }
    if (win == None) {
        XDeleteProperty(g_dpy, g_root, atom);
        g_active = 0;
    } else {
        XChangeProperty(g_dpy, g_root, atom, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&win, 1);
        g_active = 1;
    }
    g_shown = win;
    g_renewed_ms = now;
    XFlush(g_dpy);
}

void peek_set(Window win)
{
    if (win == g_hover) {
        return;
    }
    uint64_t now = now_ms();
    g_hover = win;
    g_since_ms = now;

    if (win == None) {
        if (g_active) {
            publish(None, now);
        }
    } else if (g_active) {
        publish(win, now);
    }
}

void peek_tick(uint64_t now)
{
    if (g_hover == None) {
        return;
    }
    if (!g_active) {
        if (now - g_since_ms >= PEEK_DELAY_MS) {
            publish(g_hover, now);
        }
    } else if (now - g_renewed_ms >= PEEK_RENEW_MS) {
        publish(g_hover, now);
    }
}

uint64_t peek_next_wake_ms(void)
{
    if (g_hover == None) {
        return 0;
    }
    return g_active ? g_renewed_ms + PEEK_RENEW_MS : g_since_ms + PEEK_DELAY_MS;
}
