/*
 * The window switcher drawn by the compositor (osd.h's TabBoxOps).
 *
 * kiwm keeps the keyboard. That is not a detail of this file, it is the
 * reason it exists: only one client can hold a grab, the Alt+Tab hold is
 * kiwm's, and kiwm is the only thing that can decide and carry out what
 * the hold meant. So nothing here takes a grab, reads a key, or moves
 * the focus -- the hold, the stepping and the commit are osd.c's, exactly
 * as they are for the plain list. All this does is say, on a property,
 * what is being offered and where the selection is.
 *
 * The protocol is kicomp's, described in its effects/cover-switch.c:
 *
 *   _KICOMP_EFFECTS   on the window owning _NET_WM_CM_Sn -- the modes it
 *                     can be asked to draw. Absent when there are none,
 *                     and its absence is the whole of the negotiation:
 *                     no compositor, a compositor built without the
 *                     effect, or one with it switched off all read the
 *                     same here, and kiwm draws its own list instead.
 *
 *   _KICOMP_SWITCHER  on the root -- state, selected index, then the
 *                     windows in the order to show them.
 *
 * Asked at the moment a hold opens rather than watched, because that is
 * the only moment the answer is used, and a compositor that starts or
 * stops between two holds should be noticed without kiwm having to track
 * it.
 */
#include "osd.h"
#include "wm.h"
#include "atoms.h"

#include <stdlib.h>
#include <string.h>

enum { SWITCHER_END = 0, SWITCHER_SHOW = 1, SWITCHER_COMMIT = 2 };

static xcb_atom_t atom_switcher(void)
{
    static xcb_atom_t a;
    if (a == XCB_ATOM_NONE)
        a = intern_atom("_KICOMP_SWITCHER");
    return a;
}

bool cover_switch_offered(void)
{
    if (wm.cm_owner == XCB_NONE)
        return false;

    static xcb_atom_t effects;
    if (effects == XCB_ATOM_NONE)
        effects = intern_atom("_KICOMP_EFFECTS");
    if (effects == XCB_ATOM_NONE)
        return false;

    xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, wm.cm_owner, effects,
                         XCB_ATOM_STRING, 0, 256), NULL);
    if (!r)
        return false;

    int len = xcb_get_property_value_length(r);
    const char *names = xcb_get_property_value(r);
    bool found = false;

    /* A space-separated list, matched whole so "cover-switch-2" would
     * not answer for "cover-switch". */
    for (int i = 0; i <= len && !found; i++) {
        static const char want[] = "cover-switch";
        int n = (int)sizeof want - 1;
        if (i + n > len)
            break;
        if (memcmp(names + i, want, (size_t)n) != 0)
            continue;
        bool left = (i == 0) || names[i - 1] == ' ';
        bool right = (i + n == len) || names[i + n] == ' ' || names[i + n] == '\0';
        found = left && right;
    }

    free(r);
    return found;
}

/* Everything kiwm has to say, in one write: a hold is a burst of steps
 * and the property is the state rather than a queue, so the compositor
 * reads whichever one it gets to and is never behind. */
static void publish(const TabBoxState *state, uint32_t what)
{
    xcb_atom_t a = atom_switcher();
    if (a == XCB_ATOM_NONE)
        return;

    if (what != SWITCHER_SHOW) {
        uint32_t end[2] = { what, 0 };
        xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root, a,
                            XCB_ATOM_CARDINAL, 32, 2, end);
        xcb_flush(wm.conn);
        return;
    }

    uint32_t buf[2 + MAX_CLIENTS];
    int n = 0;
    buf[n++] = what;
    buf[n++] = (uint32_t)state->selected;

    /* The client windows, not kiwm's frames: the compositor knows a
     * window by what the application created, and looks the frame up
     * from it. */
    for (int i = 0; i < state->count && n < (int)(sizeof buf / sizeof buf[0]); i++)
        buf[n++] = state->items[i]->window;

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root, a,
                        XCB_ATOM_CARDINAL, 32, (uint32_t)n, buf);
    xcb_flush(wm.conn);
}

static void cover_open(const TabBoxState *state)
{
    publish(state, SWITCHER_SHOW);
}

static void cover_step(const TabBoxState *state)
{
    publish(state, SWITCHER_SHOW);
}

static void cover_close(const TabBoxState *state, bool committed)
{
    publish(state, committed ? SWITCHER_COMMIT : SWITCHER_END);
}

const TabBoxOps cover_switch_tabbox_ops = {
    .name     = "cover",
    .build    = tabbox_build_default,
    .measure  = NULL,
    .paint    = NULL,
    .external = true,
    .open     = cover_open,
    .step     = cover_step,
    .close    = cover_close,
};
