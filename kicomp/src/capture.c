/* See capture.h. */
#include "capture.h"
#include "scene.h"
#include "window.h"
#include "effect.h"

#include "../../shared/xis_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SESSIONS 8
#define PROTOCOL_VERSION 1

/* STATE flags */
#define STATE_UNAVAILABLE (1u << 0)
/* TARGETS flags */
#define TARGET_AVAILABLE  (1u << 0)
/* FRAME flags */
#define FRAME_UNAVAILABLE (1u << 2)
/* STATUS errors */
enum { ERR_NONE, ERR_VERSION, ERR_DISABLED, ERR_OUTPUT, ERR_RESOURCES, ERR_DENIED };

enum { MODE_OFF, MODE_HIDE, MODE_FILL };

static struct {
    int mode;
    uint32_t color;      /* 0x00RRGGBB, for MODE_FILL */
    int max_sessions;
} cfg = { MODE_OFF, 0, 2 };

static struct {
    xcb_atom_t mark, capture, utf8;
    xcb_atom_t client, outputs, slots, hide_pid, targets, state;
    xcb_atom_t open, close, request, release, status, frame;
} A;

typedef struct {
    bool used;
    xcb_window_t win;                 /* the recorder's session window S */
    int nout;
    int out_id[MAX_OUTPUTS];          /* CompOutput.id, by session output index */
    uint32_t hide_pid;
    int slots;
    uint32_t epoch[MAX_OUTPUTS];
    uint32_t visible[MAX_OUTPUTS];
    uint32_t flags[MAX_OUTPUTS];
} Session;

static Session sessions[MAX_SESSIONS];
static int session_count;

/* ------------------------------------------------------------------ */
/* config                                                               */
/* ------------------------------------------------------------------ */

bool capture_config_key(const char *key, const char *value)
{
    if (strcmp(key, "capture_censor") == 0) {
        if (strcmp(value, "off") == 0) {
            cfg.mode = MODE_OFF;
        } else if (strcmp(value, "hide") == 0) {
            cfg.mode = MODE_HIDE;
        } else if (strcmp(value, "black") == 0) {
            cfg.mode = MODE_FILL;
            cfg.color = 0;
        } else if (value[0] == '#' && strlen(value) == 7) {
            char *end = NULL;
            unsigned long v = strtoul(value + 1, &end, 16);
            if (end && !*end) {
                cfg.mode = MODE_FILL;
                cfg.color = (uint32_t)v;
            } else {
                fprintf(stderr, "kicomp: config: bad capture_censor colour '%s'\n", value);
            }
        } else {
            fprintf(stderr, "kicomp: config: unknown capture_censor '%s'\n", value);
        }
    } else if (strcmp(key, "capture_sessions") == 0) {
        int n = atoi(value);
        cfg.max_sessions = n < 1 ? 1 : n > MAX_SESSIONS ? MAX_SESSIONS : n;
    } else {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* small X helpers                                                      */
/* ------------------------------------------------------------------ */

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(comp.conn,
        xcb_intern_atom(comp.conn, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_NONE;
    free(r);
    return a;
}

void capture_init(void)
{
    A.mark     = intern(XIS_CAPTURE_MARK_ATOM);
    A.capture  = intern("_KICOMP_CAPTURE");
    A.utf8     = intern("UTF8_STRING");
    A.client   = intern("_KICOMP_CAPTURE_CLIENT");
    A.outputs  = intern("_KICOMP_CAPTURE_OUTPUTS");
    A.slots    = intern("_KICOMP_CAPTURE_SLOTS");
    A.hide_pid = intern("_KICOMP_CAPTURE_HIDE_PID");
    A.targets  = intern("_KICOMP_CAPTURE_TARGETS");
    A.state    = intern("_KICOMP_CAPTURE_STATE");
    A.open     = intern("_KICOMP_CAPTURE_OPEN");
    A.close    = intern("_KICOMP_CAPTURE_CLOSE");
    A.request  = intern("_KICOMP_CAPTURE_REQUEST");
    A.release  = intern("_KICOMP_CAPTURE_RELEASE");
    A.status   = intern("_KICOMP_CAPTURE_STATUS");
    A.frame    = intern("_KICOMP_CAPTURE_FRAME");
}

static void set_cardinals(xcb_window_t win, xcb_atom_t atom, const uint32_t *v, uint32_t n)
{
    xcb_change_property(comp.conn, XCB_PROP_MODE_REPLACE, win, atom,
                        XCB_ATOM_CARDINAL, 32, n, v);
}

static void send_message(xcb_window_t to, xcb_atom_t type, const uint32_t d[5])
{
    xcb_client_message_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = to;
    ev.type = type;
    for (int i = 0; i < 5; i++)
        ev.data.data32[i] = d[i];
    /* An empty event mask delivers to whoever created the window, with no
     * selection needed -- the way _KICOMP_QUIT reaches us. */
    xcb_send_event(comp.conn, 0, to, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void send_status(xcb_window_t to, uint32_t state, uint32_t error, uint32_t outputs)
{
    uint32_t d[5] = { state, error, outputs, PROTOCOL_VERSION, 0 };
    send_message(to, A.status, d);
}

/* ------------------------------------------------------------------ */
/* the mark                                                             */
/* ------------------------------------------------------------------ */

xcb_get_property_cookie_t capture_mark_request(xcb_window_t client)
{
    return xcb_get_property(comp.conn, 0, client, A.mark, XCB_ATOM_CARDINAL, 0, 1);
}

bool capture_mark_reply(xcb_get_property_cookie_t cookie)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn, cookie, NULL);
    bool set = false;
    if (r) {
        if (r->type == XCB_ATOM_CARDINAL && r->format == 32 &&
            xcb_get_property_value_length(r) >= 4)
            set = *(uint32_t *)xcb_get_property_value(r) != 0;
        free(r);
    }
    return set;
}

bool capture_property_notify(xcb_window_t window, xcb_atom_t atom)
{
    if (atom != A.mark || A.mark == XCB_NONE)
        return false;

    /* On the client, which is not the window the compositor tracks. */
    CompWindow *w = window_find_by_client(window);
    if (!w)
        w = window_find(window);
    if (w)
        w->capture_marked = capture_mark_reply(capture_mark_request(window));
    return true;
}

/* Marked, or a popup of a marked window (shared/xis_capture.h). */
static void recompute_excluded(void)
{
    uint32_t pids[64];
    size_t n = 0;
    for (CompWindow *w = comp.stack; w; w = w->next)
        if (w->capture_marked && !w->zombie && w->pid && n < 64)
            pids[n++] = w->pid;

    for (CompWindow *w = comp.stack; w; w = w->next)
        w->capture_excluded = w->capture_marked ||
            xis_capture_popup_excluded(w->override_redirect, w->pid, pids, n);
}

/* ------------------------------------------------------------------ */
/* what a session sees                                                  */
/* ------------------------------------------------------------------ */

static const CompOutput *output_by_id(int id)
{
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == id)
            return &comp.outputs[i];
    return NULL;
}

/* Excluded windows contributing pixels to `o`. Over-counting is safe (the
 * recorder just censors that output itself); under-counting is the one
 * thing that must not happen. So while any effect runs, a window the
 * scene would draw counts on every output -- an effect may be drawing it
 * anywhere. */
static uint32_t count_visible(const Session *s, const CompOutput *o)
{
    uint32_t n = 0;
    for (const CompWindow *w = comp.stack; w; w = w->next) {
        bool ex = w->capture_excluded || (s->hide_pid && w->pid == s->hide_pid);
        if (!ex || !scene_wants_window(w, o))
            continue;
        CompRect g = window_rect((CompWindow *)w), hit;
        if (rect_intersect(&g, &o->rect, &hit) || effects_active())
            n++;
    }
    return n;
}

/* Whether a censored picture of this output could be rendered right now.
 * The renderers do not do the second assembly yet (REQUEST/FRAME), so no. */
static bool can_render_censored(const CompOutput *o)
{
    (void)o;
    return false;
}

static void write_targets(const Session *s)
{
    uint32_t rec[MAX_OUTPUTS * 13];
    memset(rec, 0, sizeof rec);
    for (int i = 0; i < s->nout; i++) {
        const CompOutput *o = output_by_id(s->out_id[i]);
        uint32_t *r = &rec[i * 13];
        if (!o)
            continue;      /* gone: all zero, not AVAILABLE */
        r[0] = can_render_censored(o) ? TARGET_AVAILABLE : 0;
        r[1] = (uint32_t)o->rect.x;
        r[2] = (uint32_t)o->rect.y;
        r[3] = (uint32_t)o->rect.w;
        r[4] = (uint32_t)o->rect.h;
        r[5] = (uint32_t)o->physical.w;
        r[6] = (uint32_t)o->physical.h;
        r[7] = 24;
        r[8] = 0;          /* slot count: none allocated until the renderers can fill them */
    }
    set_cardinals(s->win, A.targets, rec, (uint32_t)s->nout * 13);
}

static void write_state(const Session *s)
{
    uint32_t words[MAX_OUTPUTS * 3];
    for (int i = 0; i < s->nout; i++) {
        words[i * 3 + 0] = s->epoch[i];
        words[i * 3 + 1] = s->visible[i];
        words[i * 3 + 2] = s->flags[i];
    }
    set_cardinals(s->win, A.state, words, (uint32_t)s->nout * 3);
}

/* Recomputes one session's STATE and writes it if anything changed (or
 * `force`). Returns whether it was written. */
static bool update_session(Session *s, bool force)
{
    bool changed = force;
    for (int i = 0; i < s->nout; i++) {
        const CompOutput *o = output_by_id(s->out_id[i]);
        uint32_t v = o ? count_visible(s, o) : 0;
        uint32_t f = 0;
        if (!o || (v > 0 && !can_render_censored(o)))
            f |= STATE_UNAVAILABLE;

        if ((v != 0) != (s->visible[i] != 0)) {
            s->epoch[i]++;
            changed = true;
        }
        if (v != s->visible[i] || f != s->flags[i])
            changed = true;
        s->visible[i] = v;
        s->flags[i] = f;
    }
    if (changed)
        write_state(s);
    return changed;
}

void capture_update(void)
{
    if (session_count == 0)
        return;

    recompute_excluded();
    bool wrote = false;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used)
            wrote |= update_session(&sessions[i], false);
    if (wrote)
        xcb_flush(comp.conn);
}

bool capture_blocks_unredirect(const CompOutput *o)
{
    if (session_count == 0)
        return false;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const Session *s = &sessions[i];
        if (!s->used)
            continue;
        for (int k = 0; k < s->nout; k++)
            if (s->out_id[k] == o->id && s->visible[k] > 0)
                return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* sessions                                                             */
/* ------------------------------------------------------------------ */

static Session *session_for(xcb_window_t win)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && sessions[i].win == win)
            return &sessions[i];
    return NULL;
}

/* Properties first, then (when asked) the message that says it is over;
 * the pixmaps would be freed after the properties go, for the reason
 * _KICOMP_STOWED_PIXMAP gives (there are none yet). */
static void end_session(Session *s, bool tell)
{
    xcb_delete_property(comp.conn, s->win, A.targets);
    xcb_delete_property(comp.conn, s->win, A.state);
    if (tell)
        send_status(s->win, 0, ERR_NONE, 0);
    uint32_t none = 0;
    xcb_change_window_attributes(comp.conn, s->win, XCB_CW_EVENT_MASK, &none);
    memset(s, 0, sizeof *s);
    session_count--;
    xcb_flush(comp.conn);
}

static xcb_get_property_reply_t *get(xcb_window_t win, xcb_atom_t atom, xcb_atom_t type, uint32_t len)
{
    return xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, win, atom, type, 0, len), NULL);
}

static void handle_open(xcb_window_t win, uint32_t version)
{
    if (cfg.mode == MODE_OFF) {
        send_status(win, 0, ERR_DISABLED, 0);
        return;
    }
    if (version != PROTOCOL_VERSION) {
        send_status(win, 0, ERR_VERSION, 0);
        return;
    }
    Session *have = session_for(win);
    if (have) {      /* asked twice: answer again, change nothing */
        send_status(win, 1, ERR_NONE, (uint32_t)have->nout);
        return;
    }
    if (session_count >= cfg.max_sessions) {
        send_status(win, 0, ERR_RESOURCES, 0);
        return;
    }

    Session s;
    memset(&s, 0, sizeof s);
    s.win = win;
    s.slots = 3;

    xcb_get_property_reply_t *r = get(win, A.outputs, A.utf8, 256);
    int len = r ? xcb_get_property_value_length(r) : 0;
    const char *v = r ? xcb_get_property_value(r) : NULL;
    bool bad = (len <= 0);
    for (int pos = 0; !bad && pos < len; ) {
        size_t nl = strnlen(v + pos, (size_t)(len - pos));
        if (nl > 0) {
            int id = -1;
            for (int i = 0; i < comp.output_count; i++)
                if (strlen(comp.outputs[i].name) == nl &&
                    strncmp(comp.outputs[i].name, v + pos, nl) == 0)
                    id = comp.outputs[i].id;
            if (id < 0 || s.nout >= MAX_OUTPUTS)
                bad = true;
            else
                s.out_id[s.nout++] = id;
        }
        pos += (int)nl + 1;
    }
    free(r);
    if (bad || s.nout == 0) {
        send_status(win, 0, ERR_OUTPUT, 0);
        return;
    }

    r = get(win, A.slots, XCB_ATOM_CARDINAL, 1);
    if (r && xcb_get_property_value_length(r) >= 4) {
        uint32_t n = *(uint32_t *)xcb_get_property_value(r);
        s.slots = n < 1 ? 1 : n > 4 ? 4 : (int)n;
    }
    free(r);
    r = get(win, A.hide_pid, XCB_ATOM_CARDINAL, 1);
    if (r && xcb_get_property_value_length(r) >= 4)
        s.hide_pid = *(uint32_t *)xcb_get_property_value(r);
    free(r);

    /* We need to hear about S going away -- a recorder that dies costs
     * nothing afterwards. A checked request: a bogus window id is the one
     * way OPEN can fail on us, and then there is nobody to answer. */
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_void_cookie_t ck = xcb_change_window_attributes_checked(comp.conn, win,
                                                                XCB_CW_EVENT_MASK, &mask);
    xcb_generic_error_t *err = xcb_request_check(comp.conn, ck);
    if (err) {
        free(err);
        return;
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].used)
            continue;
        sessions[i] = s;
        sessions[i].used = true;
        for (int k = 0; k < s.nout; k++)
            sessions[i].epoch[k] = 1;
        session_count++;
        recompute_excluded();
        /* Properties first, then the message that points at them. */
        write_targets(&sessions[i]);
        update_session(&sessions[i], true);
        send_status(win, 1, ERR_NONE, (uint32_t)s.nout);
        xcb_flush(comp.conn);
        comp_log("capture: session on 0x%x, %d output(s)", win, s.nout);
        return;
    }
}

bool capture_client_message(const xcb_client_message_event_t *e)
{
    if (e->type == XCB_NONE ||
        (e->type != A.open && e->type != A.close && e->type != A.request &&
         e->type != A.release))
        return false;

    xcb_window_t win = e->data.data32[0];
    if (e->type == A.open) {
        handle_open(win, e->data.data32[1]);
        return true;
    }

    Session *s = session_for(win);
    if (!s)
        return true;
    if (e->type == A.close) {
        end_session(s, false);
    } else if (e->type == A.request) {
        /* Nothing renders the second picture yet: answer, as the protocol
         * requires, with the failure that sends the recorder to its own
         * censoring. */
        uint32_t d[5] = { e->data.data32[1], 0xFFFFFFFFu, e->data.data32[2],
                          FRAME_UNAVAILABLE, 0 };
        send_message(win, A.frame, d);
        xcb_flush(comp.conn);
    }
    /* RELEASE: no slots are ever held. */
    return true;
}

void capture_window_destroyed(xcb_window_t window)
{
    Session *s = session_for(window);
    if (s) {
        comp_log("capture: session window 0x%x gone", window);
        memset(s, 0, sizeof *s);
        session_count--;
    }
}

void capture_outputs_changed(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used)
            continue;
        write_targets(&sessions[i]);
        update_session(&sessions[i], true);
    }
    xcb_flush(comp.conn);
}

/* ------------------------------------------------------------------ */
/* the offer                                                            */
/* ------------------------------------------------------------------ */

void capture_publish(void)
{
    if (cfg.mode == MODE_OFF || A.capture == XCB_NONE || comp.cm_window == XCB_NONE)
        return;
    /* version, features (CENSOR), mode (0 hide, 1 fill), colour */
    uint32_t v[4] = { PROTOCOL_VERSION, 1u << 0, cfg.mode == MODE_FILL ? 1u : 0u, cfg.color };
    set_cardinals(comp.cm_window, A.capture, v, 4);
    comp_info("capture: offered (%s)", cfg.mode == MODE_FILL ? "fill" : "hide");
}

void capture_shutdown(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used)
            end_session(&sessions[i], true);
    if (comp.cm_window != XCB_NONE && A.capture != XCB_NONE)
        xcb_delete_property(comp.conn, comp.cm_window, A.capture);
}
