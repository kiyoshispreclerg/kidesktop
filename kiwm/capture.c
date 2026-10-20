/* capture.c - kiwm's part of the capture-exclusion protocol (PROTOCOL.md's
 * _KIWM_CAPTURE_EXCLUDE): setting and clearing the mark, marking by class
 * before a window is mapped, and handing the mark down to dialogs.
 *
 * Nothing is cached on the Client: the property is the state, so a tool that
 * sets or deletes it behind kiwm's back (kishot's "Hide a Window...") is
 * respected the next time anything here asks. */
#include "capture.h"
#include "client.h"

#include <xcb/xcb_icccm.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

int capture_mark_get(xcb_window_t window)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.kiwm_capture_exclude, XCB_ATOM_CARDINAL, 0, 1), NULL);
    int v = 0;
    if (r) {
        if (r->type == XCB_ATOM_CARDINAL && r->format == 32 && xcb_get_property_value_length(r) >= 4)
            v = (int)*(uint32_t *)xcb_get_property_value(r);
        free(r);
    }
    return v;
}

static void mark_set(xcb_window_t window, uint32_t value)
{
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, window, wm.atoms.kiwm_capture_exclude,
                        XCB_ATOM_CARDINAL, 32, 1, &value);
}

static void mark_clear(xcb_window_t window)
{
    xcb_delete_property(wm.conn, window, wm.atoms.kiwm_capture_exclude);
}

/* Is `c` a dialog -- directly or up the chain -- of a window whose mark is
 * `want` (1: marked itself) or of any marked window (want 0)? */
static bool has_marked_ancestor(const Client *c, int want)
{
    xcb_window_t parent = c->transient_for;
    for (int guard = 0; parent != XCB_NONE && guard < MAX_CLIENTS; guard++) {
        const Client *p = find_client_window(parent);
        if (!p)
            return false;
        int v = capture_mark_get(p->window);
        if (want ? v == want : v != 0)
            return true;
        parent = p->transient_for;
    }
    return false;
}

static bool is_descendant_of(const Client *c, const Client *root)
{
    xcb_window_t parent = c->transient_for;
    for (int guard = 0; parent != XCB_NONE && guard < MAX_CLIENTS; guard++) {
        if (parent == root->window)
            return true;
        const Client *p = find_client_window(parent);
        if (!p)
            return false;
        parent = p->transient_for;
    }
    return false;
}

void capture_toggle(Client *c)
{
    if (capture_mark_get(c->window) == 0) {
        mark_set(c->window, 1);
        for (Client *d = wm.clients; d; d = d->next)
            if (d != c && is_descendant_of(d, c) && capture_mark_get(d->window) == 0)
                mark_set(d->window, 2);
    } else {
        mark_clear(c->window);
        /* What was inherited stays only where another marked-1 ancestor
         * still hands it down. */
        for (Client *d = wm.clients; d; d = d->next)
            if (capture_mark_get(d->window) == 2 && !has_marked_ancestor(d, 1))
                mark_clear(d->window);
    }
    xcb_flush(wm.conn);
}

/* kiwm.conf's capture_exclude_classes=: comma-separated, instance or class,
 * case-insensitive. */
static bool class_listed(xcb_window_t window)
{
    const char *list = wm.capture_exclude_classes;
    if (!*list)
        return false;
    xcb_icccm_get_wm_class_reply_t cls;
    if (!xcb_icccm_get_wm_class_reply(wm.conn, xcb_icccm_get_wm_class(wm.conn, window), &cls, NULL))
        return false;
    bool hit = false;
    const char *p = list;
    while (*p && !hit) {
        while (*p == ',' || *p == ' ' || *p == '\t')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        size_t len = (size_t)(end - start);
        if (len == 0)
            continue;
        hit = (cls.class_name && strlen(cls.class_name) == len && strncasecmp(cls.class_name, start, len) == 0) ||
              (cls.instance_name && strlen(cls.instance_name) == len &&
               strncasecmp(cls.instance_name, start, len) == 0);
    }
    xcb_icccm_get_wm_class_reply_wipe(&cls);
    return hit;
}

void capture_mark_new_client(Client *c)
{
    if (capture_mark_get(c->window) != 0)
        return;                     /* it came marked (an app or a tool did it) */
    if (class_listed(c->window))
        mark_set(c->window, 1);
    else if (has_marked_ancestor(c, 0))
        mark_set(c->window, 2);
}

void capture_refresh_inherited(Client *c)
{
    int v = capture_mark_get(c->window);
    if (v == 0 && has_marked_ancestor(c, 0))
        mark_set(c->window, 2);
    else if (v == 2 && !has_marked_ancestor(c, 1))
        mark_clear(c->window);
    xcb_flush(wm.conn);
}
