/* ks_capture.c - the recorder's side of the compositor's capture protocol
 * (kicomp/CAPTURE-PROTOCOL.md): discovery, a session, and the per-frame
 * question "is anything that should be hidden showing on the part of the
 * screen I am about to copy?".
 *
 * What this does today: when the compositor offers the protocol, kishot opens
 * a session and reads _KICOMP_CAPTURE_STATE. If the compositor says no
 * excluded window contributes pixels to the outputs under the area, the
 * screen *is* the censored picture and kishot skips painting its own black
 * rectangles -- the seqlock below makes that safe against a window showing
 * up between the read and the copy. In every other case (no compositor, no
 * offer, a refused session, `visible` > 0, UNAVAILABLE, a changed epoch) it
 * censors the way it always has. Reading a censored picture from a slot
 * (REQUEST/FRAME) is the next step and not here yet.
 *
 * The control connection (OPEN, STATUS, the CM window's fate) is ours alone;
 * the per-frame STATE reads go over the connection that does the copy,
 * because the seqlock relies on the server handling a connection's requests
 * in order. A session is kept open for a few seconds after its last user, so
 * a burst of preview frames does not open and close it each time. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkx.h>
#include <X11/Xlib-xcb.h>
#include <poll.h>
#include <string.h>

#define IDLE_CLOSE_S 3
#define OPEN_TIMEOUT_MS 400

static struct {
    GMutex lock;
    xcb_connection_t *c;
    xcb_window_t root;
    int screen;
    xcb_atom_t cm_sel, capture, utf8, outputs, slots, client, targets, state, open, close, status;
    xcb_window_t cm, s;          /* 0 = none */
    gboolean active;             /* the session is open and the compositor still there */
    int nout;
    KsRect rect[KS_CAP_MAX_OUT];
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
        if (e->type == cap.status && e->data.data32[0] == 0)
            cap.active = FALSE;                 /* ended by the compositor */
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
    uint32_t one = 1;      /* no slots are read yet: a single one is all a screenshot asks for */
    xcb_change_property(cap.c, XCB_PROP_MODE_REPLACE, cap.s, cap.slots, XCB_ATOM_CARDINAL, 32, 1, &one);
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
    gboolean clear = TRUE;
    for (int i = 0; i < sn.nout; i++) {
        if (!intersects(area, &sn.rect[i]))
            continue;
        if ((st[i * 3 + 2] & 1) || st[i * 3 + 1] != 0)      /* UNAVAILABLE, or something excluded shows */
            clear = FALSE;
        v->out[v->n] = i;
        v->epoch[v->n] = st[i * 3];
        v->n++;
    }
    g_free(st);
    v->clear = clear && v->n > 0;
}

gboolean ks_capture_unchanged(xcb_connection_t *c, const KsCapView *v)
{
    if (!v->clear)
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
        if (i >= sn.nout || st[i * 3] != v->epoch[k] || st[i * 3 + 1] != 0 || (st[i * 3 + 2] & 1))
            same = FALSE;
    }
    g_free(st);
    return same;
}

void ks_capture_note(gboolean clear)
{
    static int debug = -1;
    if (debug < 0)
        debug = g_getenv("KISHOT_DEBUG") != NULL;
    if (debug)
        fprintf(stderr, "kishot: capture: %s\n",
                clear ? "the screen is the censored picture" : "own black rectangles");
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
