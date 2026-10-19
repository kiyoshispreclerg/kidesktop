/* ks_capture.c - the recorder's side of the compositor's capture protocol
 * (kicomp/CAPTURE-PROTOCOL.md): discovery, a session, and the per-frame
 * question "is anything that should be hidden showing on the part of the
 * screen I am about to copy?".
 *
 * When the compositor offers the protocol, kishot opens a session and reads
 * _KICOMP_CAPTURE_STATE. For each output under the area:
 *   - nothing hidden shows: the screen *is* the censored picture and is
 *     copied as always; a seqlock (STATE read before and after, over the
 *     connection that copies) makes that safe against a window showing up in
 *     between;
 *   - something hidden shows and the compositor can render it: kishot asks
 *     for the censored picture (REQUEST), waits for the slot (FRAME), copies
 *     the area out of it and gives it back (RELEASE);
 *   - anything else -- no compositor, no offer, a refused session,
 *     UNAVAILABLE, a scaled output, a slot that did not come in time, a
 *     changed epoch -- kishot censors with its own rectangles as before.
 *
 * The control connection (OPEN, STATUS, FRAME, the CM window's fate) is ours
 * alone; the per-frame STATE reads go over the connection that does the copy,
 * because the seqlock relies on the server handling a connection's requests
 * in order. A session is kept open for a few seconds after its last user, so
 * a burst of preview frames does not open and close it each time. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkx.h>
#include <X11/Xlib-xcb.h>
#include <poll.h>
#include <X11/Xlib.h>
#include <string.h>

#define IDLE_CLOSE_S 3
#define OPEN_TIMEOUT_MS 400

static struct {
    GMutex lock;
    xcb_connection_t *c;
    xcb_window_t root;
    int screen;
    xcb_atom_t cm_sel, capture, utf8, outputs, slots, client, targets, state, open, close, status;
    xcb_atom_t request, release, frame;
    xcb_window_t cm, s;          /* 0 = none */
    gboolean active;             /* the session is open and the compositor still there */
    int nout;
    KsRect rect[KS_CAP_MAX_OUT];
    /* the rest of TARGETS: can this output be rendered censored, at what
     * size in pixels, and the slots' pixmaps */
    gboolean avail[KS_CAP_MAX_OUT];
    int pw[KS_CAP_MAX_OUT], ph[KS_CAP_MAX_OUT], nslots[KS_CAP_MAX_OUT];
    xcb_pixmap_t slotxid[KS_CAP_MAX_OUT][4];
    GMutex req_lock;             /* one request at a time: the waiter owns the wait below */
    struct { uint32_t id; gboolean got; uint32_t out, slot, flags; } wait;
    uint32_t next_id;
    int mode;                    /* 0 hide, 1 fill */
    uint32_t color;
    int refs;
    guint idle_id;
} cap;

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(cap.c,
        xcb_intern_atom(cap.c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

static gboolean connect_locked(void)
{
    if (cap.c)
        return TRUE;
    xcb_connection_t *c = xcb_connect(NULL, &cap.screen);
    if (xcb_connection_has_error(c)) {
        xcb_disconnect(c);
        return FALSE;
    }
    cap.c = c;
    cap.root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    {
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
        for (int i = 0; i < cap.screen; i++)
            xcb_screen_next(&it);
        cap.root = it.data->root;
    }
    char sel[32];
    g_snprintf(sel, sizeof sel, "_NET_WM_CM_S%d", cap.screen);
    cap.cm_sel   = intern(sel);
    cap.capture  = intern("_KICOMP_CAPTURE");
    cap.utf8     = intern("UTF8_STRING");
    cap.outputs  = intern("_KICOMP_CAPTURE_OUTPUTS");
    cap.slots    = intern("_KICOMP_CAPTURE_SLOTS");
    cap.client   = intern("_KICOMP_CAPTURE_CLIENT");
    cap.targets  = intern("_KICOMP_CAPTURE_TARGETS");
    cap.state    = intern("_KICOMP_CAPTURE_STATE");
    cap.open     = intern("_KICOMP_CAPTURE_OPEN");
    cap.close    = intern("_KICOMP_CAPTURE_CLOSE");
    cap.status   = intern("_KICOMP_CAPTURE_STATUS");
    cap.request  = intern("_KICOMP_CAPTURE_REQUEST");
    cap.release  = intern("_KICOMP_CAPTURE_RELEASE");
    cap.frame    = intern("_KICOMP_CAPTURE_FRAME");
    return TRUE;
}

/* ---- discovery ------------------------------------------------------------ */

/* The CM window and what it offers, or FALSE: no compositor, or one that does
 * not speak (this version of) the protocol. Absence is the whole answer. */
static gboolean discover_locked(xcb_window_t *cm, int *mode, uint32_t *color)
{
    if (!connect_locked() || cap.cm_sel == XCB_NONE)
        return FALSE;
    xcb_get_selection_owner_reply_t *o =
        xcb_get_selection_owner_reply(cap.c, xcb_get_selection_owner(cap.c, cap.cm_sel), NULL);
    xcb_window_t w = o ? o->owner : XCB_NONE;
    free(o);
    if (w == XCB_NONE)
        return FALSE;
    xcb_get_property_reply_t *r = xcb_get_property_reply(cap.c,
        xcb_get_property(cap.c, 0, w, cap.capture, XCB_ATOM_CARDINAL, 0, 4), NULL);
    gboolean ok = r && r->type == XCB_ATOM_CARDINAL && r->format == 32 &&
                  xcb_get_property_value_length(r) >= 16 &&
                  ((uint32_t *)xcb_get_property_value(r))[0] == 1;
    if (ok) {
        const uint32_t *v = xcb_get_property_value(r);
        *cm = w;
        *mode = (int)v[2];
        *color = v[3];
    }
    free(r);
    return ok;
}

gboolean ks_capture_offered(int *mode, uint32_t *color)
{
    g_mutex_lock(&cap.lock);
    xcb_window_t cm;
    int m = 0;
    uint32_t c = 0;
    gboolean ok = discover_locked(&cm, &m, &c);
    g_mutex_unlock(&cap.lock);
    if (ok && mode)
        *mode = m;
    if (ok && color)
        *color = c;
    return ok;
}

/* ---- events on the control connection -------------------------------------- */

static void load_targets_locked(void)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(cap.c,
        xcb_get_property(cap.c, 0, cap.s, cap.targets, XCB_ATOM_CARDINAL, 0, KS_CAP_MAX_OUT * 13), NULL);
    cap.nout = 0;
    if (r && r->type == XCB_ATOM_CARDINAL) {
        const uint32_t *v = xcb_get_property_value(r);
        int n = xcb_get_property_value_length(r) / 4 / 13;
        for (int i = 0; i < n && i < KS_CAP_MAX_OUT; i++) {
            const uint32_t *t = &v[i * 13];
            cap.rect[i] = (KsRect){ (int32_t)t[1], (int32_t)t[2], (int)t[3], (int)t[4] };
            cap.avail[i] = (t[0] & 1) != 0;
            cap.pw[i] = (int)t[5];
            cap.ph[i] = (int)t[6];
            cap.nslots[i] = (int)MIN(t[8], 4u);
            for (int k = 0; k < 4; k++)
                cap.slotxid[i][k] = k < cap.nslots[i] ? t[9 + k] : XCB_NONE;
            cap.nout = i + 1;
        }
    }
    free(r);
}

static void handle_event_locked(xcb_generic_event_t *ev)
{
    switch (ev->response_type & 0x7f) {
    case XCB_DESTROY_NOTIFY:
        if (((xcb_destroy_notify_event_t *)ev)->window == cap.cm)
            cap.active = FALSE;                 /* the compositor left */
        break;
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)ev;
        if (e->window == cap.cm && e->atom == cap.capture) {
            if (e->state == XCB_PROPERTY_DELETE) {
                cap.active = FALSE;             /* switched off */
            } else {
                xcb_window_t cm;
                int m;
                uint32_t col;
                if (discover_locked(&cm, &m, &col) && cm == cap.cm) {
                    cap.mode = m;
                    cap.color = col;
                } else {
                    cap.active = FALSE;         /* version changed, or another compositor */
                }
            }
        } else if (e->window == cap.s && e->atom == cap.targets && cap.active) {
            load_targets_locked();
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *e = (xcb_client_message_event_t *)ev;
        if (e->type == cap.status && e->data.data32[0] == 0) {
            cap.active = FALSE;                 /* ended by the compositor */
        } else if (e->type == cap.frame && e->data.data32[2] == cap.wait.id && !cap.wait.got) {
            cap.wait.got = TRUE;
            cap.wait.out = e->data.data32[0];
            cap.wait.slot = e->data.data32[1];
            cap.wait.flags = e->data.data32[3];
        }
        break;
    }
    default:
        break;
    }
}

static void pump_locked(void)
{
    if (!cap.c)
        return;
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(cap.c))) {
        handle_event_locked(ev);
        free(ev);
    }
    if (xcb_connection_has_error(cap.c))
        cap.active = FALSE;
}

/* ---- the session ----------------------------------------------------------- */

static void close_locked(void)
{
    if (cap.s) {
        if (cap.active && cap.cm) {
            xcb_client_message_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.response_type = XCB_CLIENT_MESSAGE;
            ev.format = 32;
            ev.window = cap.cm;
            ev.type = cap.close;
            ev.data.data32[0] = cap.s;
            xcb_send_event(cap.c, 0, cap.cm, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
        }
        xcb_destroy_window(cap.c, cap.s);
        xcb_flush(cap.c);
    }
    cap.s = XCB_NONE;
    cap.cm = XCB_NONE;
    cap.active = FALSE;
    cap.nout = 0;
}

/* Main thread only (it asks GDK for the monitors' names). */
static gboolean open_locked(void)
{
    xcb_window_t cm;
    int mode = 0;
    uint32_t color = 0;
    if (!discover_locked(&cm, &mode, &color))
        return FALSE;

    GString *names = g_string_new(NULL);
    int nmon = MIN(ks_monitor_count(), KS_CAP_MAX_OUT);
    for (int i = 0; i < nmon; i++) {
        char *n = ks_monitor_name(i);
        g_string_append_len(names, n, strlen(n) + 1);     /* NUL-separated */
        g_free(n);
    }
    if (!nmon) {
        g_string_free(names, TRUE);
        return FALSE;
    }

    cap.cm = cm;
    cap.mode = mode;
    cap.color = color;
    cap.s = xcb_generate_id(cap.c);
    uint32_t evmask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(cap.c, 0, cap.s, cap.root, -10, -10, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &evmask);
    xcb_change_property(cap.c, XCB_PROP_MODE_REPLACE, cap.s, cap.outputs, cap.utf8, 8,
                        (uint32_t)names->len, names->str);
    xcb_change_property(cap.c, XCB_PROP_MODE_REPLACE, cap.s, cap.client, cap.utf8, 8, 6, "kishot");
    /* One picture is read at a time and given back before the next is
     * asked for, so two slots leave the compositor one to render into. */
    uint32_t two = 2;
    xcb_change_property(cap.c, XCB_PROP_MODE_REPLACE, cap.s, cap.slots, XCB_ATOM_CARDINAL, 32, 1, &two);
    g_string_free(names, TRUE);

    /* the compositor going away, or switching the protocol off */
    uint32_t cmmask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(cap.c, cap.cm, XCB_CW_EVENT_MASK, &cmmask);

    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = cap.cm;
    ev.type = cap.open;
    ev.data.data32[0] = cap.s;
    ev.data.data32[1] = 1;
    xcb_send_event(cap.c, 0, cap.cm, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    xcb_flush(cap.c);

    /* STATUS, with a deadline: a compositor that does not answer is one we
     * do without. */
    gint64 end = g_get_monotonic_time() + OPEN_TIMEOUT_MS * 1000;
    int fd = xcb_get_file_descriptor(cap.c);
    for (;;) {
        xcb_generic_event_t *e;
        while ((e = xcb_poll_for_event(cap.c))) {
            if ((e->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                xcb_client_message_event_t *m = (xcb_client_message_event_t *)e;
                if (m->type == cap.status) {
                    gboolean ok = m->data.data32[0] == 1;
                    free(e);
                    if (!ok) {
                        cap.active = FALSE;
                        close_locked();
                        return FALSE;
                    }
                    cap.active = TRUE;
                    load_targets_locked();
                    if (cap.nout == 0) {
                        close_locked();
                        return FALSE;
                    }
                    return TRUE;
                }
            }
            handle_event_locked(e);
            free(e);
        }
        gint64 left = end - g_get_monotonic_time();
        if (left <= 0 || xcb_connection_has_error(cap.c)) {
            close_locked();
            return FALSE;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, (int)(left / 1000) + 1);
    }
}

static gboolean idle_close(gpointer data)
{
    (void)data;
    g_mutex_lock(&cap.lock);
    cap.idle_id = 0;
    if (cap.refs == 0)
        close_locked();
    g_mutex_unlock(&cap.lock);
    return FALSE;
}

gboolean ks_capture_acquire(void)
{
    g_mutex_lock(&cap.lock);
    cap.refs++;
    if (cap.idle_id) {
        g_source_remove(cap.idle_id);
        cap.idle_id = 0;
    }
    pump_locked();
    if (!cap.active) {
        close_locked();
        open_locked();
    }
    gboolean ok = cap.active;
    g_mutex_unlock(&cap.lock);
    return ok;
}

void ks_capture_release(void)
{
    g_mutex_lock(&cap.lock);
    if (cap.refs > 0 && --cap.refs == 0 && !cap.idle_id)
        cap.idle_id = g_timeout_add_seconds(IDLE_CLOSE_S, idle_close, NULL);
    g_mutex_unlock(&cap.lock);
}

/* ---- the per-frame question ------------------------------------------------ */

typedef struct { xcb_window_t s; int nout; KsRect rect[KS_CAP_MAX_OUT]; gboolean ok; } Snap;

static Snap snapshot(void)
{
    Snap sn = { 0 };
    g_mutex_lock(&cap.lock);
    pump_locked();
    if (cap.active) {
        sn.ok = TRUE;
        sn.s = cap.s;
        sn.nout = cap.nout;
        memcpy(sn.rect, cap.rect, sizeof sn.rect);
    }
    g_mutex_unlock(&cap.lock);
    return sn;
}

static gboolean intersects(const KsRect *a, const KsRect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

/* STATE as three words per output; NULL when it cannot be read. */
static uint32_t *read_state(xcb_connection_t *c, const Snap *sn)
{
    xcb_atom_t state = cap.state;      /* interned once, never changes */
    xcb_get_property_reply_t *r = xcb_get_property_reply(c,
        xcb_get_property(c, 0, sn->s, state, XCB_ATOM_CARDINAL, 0, KS_CAP_MAX_OUT * 3), NULL);
    uint32_t *out = NULL;
    if (r && r->type == XCB_ATOM_CARDINAL && xcb_get_property_value_length(r) / 4 >= sn->nout * 3) {
        out = g_memdup2(xcb_get_property_value(r), (gsize)sn->nout * 3 * 4);
    }
    free(r);
    return out;
}

void ks_capture_view(xcb_connection_t *c, const KsRect *area, KsCapView *v)
{
    memset(v, 0, sizeof *v);
    if (!ks_censor_marks_only())
        return;                     /* kishot has targets the compositor does not know */
    Snap sn = snapshot();
    if (!sn.ok)
        return;
    uint32_t *st = read_state(c, &sn);
    if (!st)
        return;
    gboolean ok = TRUE;
    g_mutex_lock(&cap.lock);
    for (int i = 0; i < sn.nout && i < KS_CAP_MAX_OUT; i++) {
        if (!intersects(area, &sn.rect[i]))
            continue;
        gboolean slot = st[i * 3 + 1] != 0;
        if (st[i * 3 + 2] & 1)
            ok = FALSE;                                   /* UNAVAILABLE */
        else if (slot && !(cap.avail[i] && cap.nslots[i] > 0 && cap.pw[i] == sn.rect[i].w &&
                           cap.ph[i] == sn.rect[i].h))    /* a scaled output is not read from a slot */
            ok = FALSE;
        v->out[v->n] = i;
        v->epoch[v->n] = st[i * 3];
        v->via_slot[v->n] = slot;
        v->orect[v->n] = sn.rect[i];
        v->n_slot += slot;
        v->n++;
    }
    g_mutex_unlock(&cap.lock);
    g_free(st);
    v->ok = ok && v->n > 0;
}

gboolean ks_capture_unchanged(xcb_connection_t *c, const KsCapView *v)
{
    if (!v->ok)
        return FALSE;
    Snap sn = snapshot();            /* the compositor may have gone since */
    if (!sn.ok)
        return FALSE;
    uint32_t *st = read_state(c, &sn);
    if (!st)
        return FALSE;
    gboolean same = TRUE;
    for (int k = 0; k < v->n; k++) {
        int i = v->out[k];
        if (i >= sn.nout || (st[i * 3 + 2] & 1)) {
            same = FALSE;
        } else if (!v->via_slot[k] && (st[i * 3] != v->epoch[k] || st[i * 3 + 1] != 0)) {
            /* the screen was trusted here: it must not have changed */
            same = FALSE;
        }
    }
    g_free(st);
    return same;
}

/* ---- asking for the censored picture --------------------------------------- */

#define FRAME_TIMEOUT_MS 150

static void send_to_cm_locked(xcb_atom_t type, uint32_t a, uint32_t b, uint32_t c)
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = cap.cm;
    ev.type = type;
    ev.data.data32[0] = cap.s;
    ev.data.data32[1] = a;
    ev.data.data32[2] = b;
    ev.data.data32[3] = c;
    xcb_send_event(cap.c, 0, cap.cm, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
    xcb_flush(cap.c);
}

void ks_capture_return(const KsCapSlot *slots, int n)
{
    if (n <= 0)
        return;
    g_mutex_lock(&cap.lock);
    if (cap.active)
        for (int i = 0; i < n; i++)
            send_to_cm_locked(cap.release, (uint32_t)slots[i].out, (uint32_t)slots[i].slot, 0);
    g_mutex_unlock(&cap.lock);
}

gboolean ks_capture_fetch(const KsCapView *v, KsCapSlot *slots, int *n)
{
    *n = 0;
    g_mutex_lock(&cap.req_lock);
    gboolean ok = TRUE;
    for (int k = 0; k < v->n && ok; k++) {
        if (!v->via_slot[k])
            continue;
        g_mutex_lock(&cap.lock);
        if (!cap.active) {
            g_mutex_unlock(&cap.lock);
            ok = FALSE;
            break;
        }
        cap.wait.id = ++cap.next_id;
        cap.wait.got = FALSE;
        send_to_cm_locked(cap.request, (uint32_t)v->out[k], cap.wait.id, 0);
        gint64 end = g_get_monotonic_time() + FRAME_TIMEOUT_MS * 1000;
        int fd = xcb_get_file_descriptor(cap.c);
        for (;;) {
            pump_locked();
            if (cap.wait.got || !cap.active)
                break;
            gint64 left = end - g_get_monotonic_time();
            if (left <= 0)
                break;
            g_mutex_unlock(&cap.lock);
            struct pollfd p = { fd, POLLIN, 0 };
            poll(&p, 1, (int)(left / 1000) + 1);
            g_mutex_lock(&cap.lock);
        }
        if (g_getenv("KISHOT_DEBUG"))
            fprintf(stderr, "kishot: capture: FRAME %s after %.1f ms\n", cap.wait.got ? "arrived" : "timed out",
                    (FRAME_TIMEOUT_MS * 1000 - (end - g_get_monotonic_time())) / 1000.0);
        gboolean got = cap.wait.got && cap.active;
        uint32_t out = cap.wait.out, slot = cap.wait.slot, flags = cap.wait.flags;
        cap.wait.id = 0;
        if (!got) {
            ok = FALSE;       /* late: its FRAME, if it comes, is held by nobody we know of */
        } else if (slot == 0xFFFFFFFFu || (flags & 6) || slot >= 4 || (int)out >= cap.nout ||
                   cap.slotxid[out][slot] == XCB_NONE) {
            ok = FALSE;       /* BUSY or UNAVAILABLE: nothing is held */
        } else {
            slots[*n] = (KsCapSlot){ (int)out, (int)slot, cap.slotxid[out][slot], v->orect[k] };
            (*n)++;
        }
        g_mutex_unlock(&cap.lock);
    }
    if (!ok) {
        ks_capture_return(slots, *n);
        *n = 0;
    }
    g_mutex_unlock(&cap.req_lock);
    return ok;
}

/* ---- reading the slots ------------------------------------------------------- */

static gboolean slot_part(const KsCapSlot *s, const KsRect *area, KsRect *src, KsRect *dst)
{
    int x1 = MAX(area->x, s->orect.x), y1 = MAX(area->y, s->orect.y);
    int x2 = MIN(area->x + area->w, s->orect.x + s->orect.w);
    int y2 = MIN(area->y + area->h, s->orect.y + s->orect.h);
    if (x2 <= x1 || y2 <= y1)
        return FALSE;
    *src = (KsRect){ x1 - s->orect.x, y1 - s->orect.y, x2 - x1, y2 - y1 };
    *dst = (KsRect){ x1 - area->x, y1 - area->y, x2 - x1, y2 - y1 };
    return TRUE;
}

void ks_capture_copy_xcb(xcb_connection_t *c, const KsCapSlot *s, int n, const KsRect *area,
                         xcb_drawable_t dst, xcb_gcontext_t gc)
{
    for (int i = 0; i < n; i++) {
        KsRect a, b;
        if (slot_part(&s[i], area, &a, &b))
            xcb_copy_area(c, s[i].pixmap, dst, gc, a.x, a.y, b.x, b.y, b.w, b.h);
    }
}

void ks_capture_copy_xlib(void *display, const KsCapSlot *s, int n, const KsRect *area,
                          unsigned long dst, void *gc)
{
    for (int i = 0; i < n; i++) {
        KsRect a, b;
        if (slot_part(&s[i], area, &a, &b))
            XCopyArea(display, s[i].pixmap, dst, gc, a.x, a.y, a.w, a.h, b.x, b.y);
    }
}

gboolean ks_capture_copy_buffer(xcb_connection_t *c, const KsCapSlot *s, int n, const KsRect *area,
                                uint8_t *buf, int stride)
{
    for (int i = 0; i < n; i++) {
        KsRect a, b;
        if (!slot_part(&s[i], area, &a, &b))
            continue;
        xcb_get_image_reply_t *im = xcb_get_image_reply(c,
            xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, s[i].pixmap, a.x, a.y, a.w, a.h, ~0u), NULL);
        if (!im)
            return FALSE;
        const uint8_t *src = xcb_get_image_data(im);
        for (int y = 0; y < a.h; y++)
            memcpy(buf + (size_t)(b.y + y) * stride + (size_t)b.x * 4, src + (size_t)y * a.w * 4,
                   (size_t)a.w * 4);
        free(im);
    }
    return TRUE;
}

gboolean ks_capture_copy_pixbuf(const KsCapSlot *s, int n, const KsRect *area, GdkPixbuf *pb)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    int nch = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    gboolean ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        KsRect a, b;
        if (!slot_part(&s[i], area, &a, &b))
            continue;
        gdk_error_trap_push();
        XImage *im = XGetImage(dpy, s[i].pixmap, a.x, a.y, a.w, a.h, AllPlanes, ZPixmap);
        gdk_flush();
        if (gdk_error_trap_pop() || !im || im->bits_per_pixel != 32) {
            ok = FALSE;
        } else {
            for (int y = 0; y < a.h; y++) {
                const uint32_t *src = (const uint32_t *)(im->data + y * im->bytes_per_line);
                guchar *d = px + (b.y + y) * stride + b.x * nch;
                for (int x = 0; x < a.w; x++, d += nch) {
                    d[0] = src[x] >> 16;
                    d[1] = src[x] >> 8;
                    d[2] = src[x];
                    if (nch == 4)
                        d[3] = 255;
                }
            }
        }
        if (im)
            XDestroyImage(im);
    }
    return ok;
}

void ks_capture_note(const KsCapView *v, gboolean censored, gboolean slots)
{
    static int debug = -1;
    if (debug < 0)
        debug = g_getenv("KISHOT_DEBUG") != NULL;
    if (!debug)
        return;
    (void)v;
    fprintf(stderr, "kishot: capture: %s\n",
            !censored ? "own black rectangles"
                      : slots ? "the compositor's censored picture (slot)"
                              : "the screen is the censored picture");
}

xcb_connection_t *ks_capture_gdk_connection(void)
{
    return XGetXCBConnection(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));
}

/* What the main window tells the user. */
char *ks_capture_status(void)
{
    int mode = 0;
    if (!ks_capture_offered(&mode, NULL)) {
        return g_strdup(ks_censor_compositor() ? _("Compositor without capture support: black rectangles")
                                               : _("No compositor: black rectangles"));
    }
    return g_strdup(mode ? _("Compositor (fill): black rectangles")
                         : _("Compositor (hide): black rectangles"));
}
