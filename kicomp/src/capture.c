/* See capture.h. */
#include "capture.h"
#include "scene.h"
#include "window.h"
#include "effect.h"
#include "renderer.h"
#include "animation.h"
#include "output.h"
#include "unredirect.h"
#include "shadow.h"
#include "region.h"

#include "../../shared/xis_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SESSIONS 8
#define MAX_SLOTS 4
#define SLOT_NONE 0xFFFFFFFFu
#define PROTOCOL_VERSION 1

/* STATE flags */
#define STATE_UNAVAILABLE (1u << 0)
/* TARGETS flags */
#define TARGET_AVAILABLE  (1u << 0)
/* FRAME flags */
#define FRAME_UNCHANGED   (1u << 0)
#define FRAME_BUSY        (1u << 1)
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

    /* Per session output: the slots (pixmaps of the output's physical
     * size, ours; the recorder only reads them), how many times each is
     * held by the recorder (FRAME delivered, RELEASE not yet), when it was
     * last delivered (the least recently delivered free one is rendered
     * into next), and the picture last made: which slot, and what the
     * output and the excluded set looked like then (for UNCHANGED). */
    int nslot;
    xcb_pixmap_t slot[MAX_OUTPUTS][MAX_SLOTS];
    uint32_t held[MAX_OUTPUTS][MAX_SLOTS];
    uint64_t delivered[MAX_OUTPUTS][MAX_SLOTS];
    bool pending[MAX_OUTPUTS];         /* a REQUEST waits for the next paint */
    uint32_t pending_id[MAX_OUTPUTS];
    bool have_pic[MAX_OUTPUTS];
    int pic_slot[MAX_OUTPUTS];
    uint64_t pic_serial[MAX_OUTPUTS];
    uint64_t pic_gen[MAX_OUTPUTS];
    double req_ms[MAX_OUTPUTS];        /* when the pending REQUEST came in */
    /* What changed on the output since each slot was last brought up to
     * date: the next picture into that slot redraws only this, the slot
     * keeping the rest -- the real frame's partial repaint, per slot. Full
     * for a slot never drawn. */
    CompRegion stale[MAX_OUTPUTS][MAX_SLOTS];
    /* The slot holds the flat picture of an output handed to an excluded
     * fullscreen window: that picture does not depend on the window's
     * contents, so its damage (a game redrawing every frame) is not stale. */
    bool flat[MAX_OUTPUTS][MAX_SLOTS];
} Session;

/* Bumped whenever the set of excluded windows changes, so a picture made
 * before is never called UNCHANGED after. */
static uint64_t excluded_gen = 1;
static uint64_t delivery_stamp;

double comp_capture_timing[3];

/* KICOMP_CAPDBG=1: averages of where a censored picture's time goes, every
 * 120 pictures. */
static struct { int n; double wait, build, draw, read, put, total, area; } stat;
static int capdbg = -1;

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

static void damage_window_everywhere(const CompWindow *w);

/* Marked, or a popup of a marked window (shared/xis_capture.h). */
static bool recompute_excluded(void)
{
    bool changed = false;
    uint32_t pids[64];
    size_t n = 0;
    for (CompWindow *w = comp.stack; w; w = w->next)
        if (w->capture_marked && !w->zombie && w->pid && n < 64)
            pids[n++] = w->pid;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        bool ex = w->capture_marked ||
            xis_capture_popup_excluded(w->override_redirect, w->pid, pids, n);
        if (ex != w->capture_excluded) {
            w->capture_excluded = ex;
            changed = true;
            damage_window_everywhere(w);
        }
    }
    if (changed)
        excluded_gen++;
    return changed;
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
    /* An output handed to one fullscreen window (unredirect.c) shows that
     * window and nothing else: whatever is behind it is not in the picture,
     * marked or not. So what is excluded on this output is exactly whether
     * that window is. */
    const CompWindow *held = unredirect_holder(o);
    if (held)
        return (held->capture_excluded || (s->hide_pid && held->pid == s->hide_pid)) ? 1 : 0;

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

/* Whether a censored picture of session output `i` could be rendered right
 * now: the renderer does the second assembly, and the slots exist. */
static bool can_render_censored(const Session *s, int i, const CompOutput *o)
{
    return o && renderer && renderer->capture_render && o->render_data &&
           s->nslot > 0 && s->slot[i][0] != XCB_NONE;
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
        r[0] = can_render_censored(s, i, o) ? TARGET_AVAILABLE : 0;
        r[1] = (uint32_t)o->rect.x;
        r[2] = (uint32_t)o->rect.y;
        r[3] = (uint32_t)o->rect.w;
        r[4] = (uint32_t)o->rect.h;
        r[5] = (uint32_t)o->physical.w;
        r[6] = (uint32_t)o->physical.h;
        r[7] = comp.screen->root_depth;
        r[8] = (uint32_t)(can_render_censored(s, i, o) ? s->nslot : 0);
        for (int k = 0; k < s->nslot && can_render_censored(s, i, o); k++)
            r[9 + k] = s->slot[i][k];
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
        if (!o || (v > 0 && !can_render_censored(s, i, o)))
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

static void free_slot_set(xcb_pixmap_t set[][MAX_SLOTS], int nout)
{
    for (int i = 0; i < nout; i++)
        for (int k = 0; k < MAX_SLOTS; k++)
            if (set[i][k] != XCB_NONE) {
                if (renderer && renderer->capture_slot_freed)
                    renderer->capture_slot_freed(set[i][k]);
                xcb_free_pixmap(comp.conn, set[i][k]);
                set[i][k] = XCB_NONE;
            }
}

/* One pixmap per slot and output, at the output's final (physical) size.
 * All or nothing: a refused allocation frees what was made. */
static bool alloc_slot_set(const Session *s, xcb_pixmap_t set[][MAX_SLOTS])
{
    for (int i = 0; i < MAX_OUTPUTS; i++)
        for (int k = 0; k < MAX_SLOTS; k++)
            set[i][k] = XCB_NONE;
    if (s->nslot == 0)
        return true;
    for (int i = 0; i < s->nout; i++) {
        const CompOutput *o = output_by_id(s->out_id[i]);
        if (!o || o->physical.w <= 0 || o->physical.h <= 0 || !o->render_data)
            continue;
        for (int k = 0; k < s->nslot; k++) {
            xcb_pixmap_t pm = xcb_generate_id(comp.conn);
            xcb_void_cookie_t ck = xcb_create_pixmap_checked(comp.conn, comp.screen->root_depth, pm,
                comp.root, (uint16_t)o->physical.w, (uint16_t)o->physical.h);
            xcb_generic_error_t *err = xcb_request_check(comp.conn, ck);
            if (err) {
                free(err);
                free_slot_set(set, s->nout);
                return false;
            }
            set[i][k] = pm;
        }
    }
    return true;
}

static void all_stale(Session *s)
{
    for (int i = 0; i < MAX_OUTPUTS; i++)
        for (int k = 0; k < MAX_SLOTS; k++)
            region_set_full(&s->stale[i][k]);
}

void capture_damage(const CompOutput *o, const CompRect *r)
{
    if (session_count == 0)
        return;
    /* Nothing on this output is in the pictures but one flat colour. */
    const CompWindow *held = unredirect_holder(o);
    if (held && held->capture_excluded)
        return;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (!s->used || s->nslot == 0)
            continue;
        for (int k = 0; k < s->nout; k++)
            if (s->out_id[k] == o->id)
                for (int j = 0; j < s->nslot; j++)
                    region_add(&s->stale[k][j], r);
    }
}

void capture_damage_all(void)
{
    if (session_count == 0)
        return;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used)
            all_stale(&sessions[i]);
}

/* Somewhere on the screen nobody sees (under an opaque window), so the real
 * frame ignores it. A censored picture that hides the window on top shows
 * it, though: only then does it matter. */
static bool hides_anything(void);

void capture_damage_unseen(const CompRect *r)
{
    if (session_count == 0 || !hides_anything())
        return;
    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (rect_intersect(r, &comp.outputs[i].rect, &hit))
            capture_damage(&comp.outputs[i], &hit);
    }
}

/* A window that became (or stopped being) excluded looks different in the
 * pictures and the same on the screen, so nothing else damages it. */
static void damage_window_everywhere(const CompWindow *w)
{
    CompRect r = window_rect((CompWindow *)w);
    int m = shadow_margin();
    r.x -= m;
    r.y -= m;
    r.w += 2 * m;
    r.h += 2 * m;
    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (rect_intersect(&r, &comp.outputs[i].rect, &hit))
            capture_damage(&comp.outputs[i], &hit);
    }
}

/* Properties first, then (when asked) the message that says it is over;
 * the pixmaps go after the properties that name them, for the reason
 * _KICOMP_STOWED_PIXMAP gives: a reader that re-reads the property per use
 * can never hold an XID already freed. */
static void end_session(Session *s, bool tell)
{
    xcb_delete_property(comp.conn, s->win, A.targets);
    xcb_delete_property(comp.conn, s->win, A.state);
    if (tell)
        send_status(s->win, 0, ERR_NONE, 0);
    free_slot_set(s->slot, s->nout);
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
        /* Slots only where the renderer can fill them. */
        sessions[i].nslot = renderer && renderer->capture_render ? s.slots : 0;
        if (!alloc_slot_set(&sessions[i], sessions[i].slot)) {
            memset(&sessions[i], 0, sizeof sessions[i]);
            send_status(win, 0, ERR_RESOURCES, 0);
            return;
        }
        all_stale(&sessions[i]);
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

static void send_frame(Session *s, uint32_t out, uint32_t slot, uint32_t id, uint32_t flags)
{
    uint32_t d[5] = { out, slot, id, flags, 0 };
    send_message(s->win, A.frame, d);
    xcb_flush(comp.conn);
}

/* A slot handed to the recorder: held until its RELEASE. */
static void deliver(Session *s, int out, int slot, uint32_t id, uint32_t flags)
{
    s->held[out][slot]++;
    s->delivered[out][slot] = ++delivery_stamp;
    send_frame(s, (uint32_t)out, (uint32_t)slot, id, flags);
}

/* Every REQUEST gets exactly one FRAME with its id: either answered here at
 * once, or left pending for capture_serve() later in the same loop
 * iteration. Not tied to the output's paint: the picture has its own scene,
 * so waiting for a repaint (and its vblank) bought nothing but latency --
 * 16 ms of a 22 ms request, measured. */
static void handle_request(Session *s, uint32_t out, uint32_t id)
{
    if (out >= (uint32_t)s->nout) {
        send_frame(s, out, SLOT_NONE, id, FRAME_UNAVAILABLE);
        return;
    }
    const CompOutput *co = output_by_id(s->out_id[out]);
    if (!can_render_censored(s, (int)out, co)) {
        send_frame(s, out, SLOT_NONE, id, FRAME_UNAVAILABLE);
        return;
    }
    if (s->pending[out]) {            /* one at a time per output */
        send_frame(s, out, SLOT_NONE, id, FRAME_BUSY);
        return;
    }
    /* Nothing changed since the last picture for this session: the same
     * slot again (held again, released again). */
    if (s->have_pic[out] && !co->dirty && s->pic_serial[out] == co->damage_serial &&
        s->pic_gen[out] == excluded_gen) {
        deliver(s, (int)out, s->pic_slot[out], id, FRAME_UNCHANGED);
        return;
    }
    bool free_slot = false;
    for (int k = 0; k < s->nslot; k++)
        free_slot |= s->held[out][k] == 0;
    if (!free_slot) {
        send_frame(s, out, SLOT_NONE, id, FRAME_BUSY);
        return;
    }
    s->pending[out] = true;
    s->pending_id[out] = id;
    s->req_ms[out] = comp_now_ms();
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
        handle_request(s, e->data.data32[1], e->data.data32[2]);
    } else if (e->type == A.release) {
        uint32_t out = e->data.data32[1], slot = e->data.data32[2];
        if (out < (uint32_t)s->nout && slot < MAX_SLOTS && s->held[out][slot] > 0)
            s->held[out][slot]--;
    }
    return true;
}

void capture_window_destroyed(xcb_window_t window)
{
    Session *s = session_for(window);
    if (s) {
        comp_log("capture: session window 0x%x gone", window);
        free_slot_set(s->slot, s->nout);
        memset(s, 0, sizeof *s);
        session_count--;
    }
}

void capture_outputs_changed(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (!s->used)
            continue;
        /* The outputs may have changed size: new slots, announced, then
         * the old ones freed. A request waiting on the old ones fails. */
        xcb_pixmap_t fresh[MAX_OUTPUTS][MAX_SLOTS];
        xcb_pixmap_t old[MAX_OUTPUTS][MAX_SLOTS];
        memcpy(old, s->slot, sizeof old);
        if (alloc_slot_set(s, fresh))
            memcpy(s->slot, fresh, sizeof fresh);
        else
            memset(s->slot, 0, sizeof s->slot);     /* AVAILABLE clears: censored pictures off */
        for (int k = 0; k < s->nout; k++) {
            if (s->pending[k])
                send_frame(s, (uint32_t)k, SLOT_NONE, s->pending_id[k], FRAME_UNAVAILABLE);
            s->pending[k] = false;
            s->have_pic[k] = false;
            memset(s->held[k], 0, sizeof s->held[k]);
        }
        all_stale(s);
        write_targets(s);
        update_session(s, true);
        free_slot_set(old, s->nout);
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

/* ------------------------------------------------------------------ */
/* the second assembly                                                  */
/* ------------------------------------------------------------------ */

/* Not the frame's scene: that one has had its fully covered windows culled
 * (they are not drawn, so a hidden window that covered them would leave a
 * hole), and keeps state the next real frame depends on. This one is built
 * and run through the effects the same way, then drawn whole. */
static CompScene cap_scene;

static void assemble(Session *s, int out, CompOutput *o)
{
    uint32_t id = s->pending_id[out];
    s->pending[out] = false;

    /* The least recently delivered slot nobody holds. */
    int pick = -1;
    for (int k = 0; k < s->nslot; k++)
        if (s->held[out][k] == 0 && s->slot[out][k] != XCB_NONE &&
            (pick < 0 || s->delivered[out][k] < s->delivered[out][pick]))
            pick = k;
    if (pick < 0) {
        send_frame(s, (uint32_t)out, SLOT_NONE, id, FRAME_BUSY);
        return;
    }

    double t0 = comp_now_ms();
    scene_build(&cap_scene, o);
    effects_apply(&cap_scene, o);

    /* The output is handed to a fullscreen window that is excluded: the
     * screen is that window and nothing else, so the picture is one flat
     * colour -- the fill colour, or black in hide mode, where there is no
     * "what was behind it" to show (nothing behind an unredirected window
     * is being composited). Only that window's node is kept: it covers the
     * output, and nothing else would be seen. */
    const CompWindow *held = unredirect_holder(o);
    bool flat_all = held && held->capture_excluded;
    if (flat_all) {
        int keep = -1;
        for (int i = 0; i < cap_scene.count; i++)
            if (cap_scene.nodes[i].win == held)
                keep = i;
        if (keep > 0)
            cap_scene.nodes[0] = cap_scene.nodes[keep];
        cap_scene.count = keep >= 0 ? 1 : 0;
        cap_scene.solid_count = 0;
        cap_scene.chrome_count = 0;
        cap_scene.sky_count = 0;
        cap_scene.backdrop.rect = (CompRect){ 0, 0, 0, 0 };
    }
    int reach = shadow_margin();
    for (int i = 0; i < cap_scene.count; i++) {
        CompSceneNode *n = &cap_scene.nodes[i];
        CompRect probe = n->visible_rect;
        probe.x -= reach;
        probe.y -= reach;
        probe.w += reach * 2;
        probe.h += reach * 2;
        region_clear(&n->clip);
        region_add(&n->clip, &probe);
    }

    CompCaptureStyle st = {
        .hide = flat_all ? false : cfg.mode == MODE_HIDE,
        .color = (flat_all && cfg.mode == MODE_HIDE) ? 0 : cfg.color,
        .hide_pid = flat_all ? 0 : s->hide_pid,
    };
    double t1 = comp_now_ms();
    memset(comp_capture_timing, 0, sizeof comp_capture_timing);
    CompRegion *stale = &s->stale[out][pick];
    if (flat_all != s->flat[out][pick])
        region_set_full(stale);     /* the slot holds the other kind of picture */
    if (capdbg > 0) {
        double a = 0;
        if (region_is_full(stale))
            a = (double)o->rect.w * o->rect.h;
        else
            for (int k = 0; k < stale->count; k++)
                a += (double)stale->rects[k].w * stale->rects[k].h;
        stat.area += a;
    }
    if (!region_is_empty(stale) &&
        !renderer->capture_render(o, &cap_scene, s->slot[out][pick], &st, stale)) {
        region_set_full(stale);
        send_frame(s, (uint32_t)out, SLOT_NONE, id, FRAME_UNAVAILABLE);
        return;
    }
    region_clear(stale);
    s->flat[out][pick] = flat_all;
    if (capdbg < 0)
        capdbg = getenv("KICOMP_CAPDBG") != NULL;
    if (capdbg) {
        double t2 = comp_now_ms();
        stat.n++;
        stat.wait += t0 - s->req_ms[out];
        stat.build += t1 - t0;
        stat.draw += comp_capture_timing[0];
        stat.read += comp_capture_timing[1];
        stat.put += comp_capture_timing[2];
        stat.total += t2 - s->req_ms[out];
        if (stat.n == 120) {
            fprintf(stderr, "kicomp: capdbg avg over 120: wait %.2f build %.2f draw %.2f read %.2f "
                    "upload %.2f | request->frame %.2f ms | redrawn %.0f%% of the output\n",
                    stat.wait / 120, stat.build / 120, stat.draw / 120, stat.read / 120, stat.put / 120,
                    stat.total / 120, 100.0 * stat.area / 120 / ((double)o->rect.w * o->rect.h));
            memset(&stat, 0, sizeof stat);
        }
    }

    s->have_pic[out] = true;
    s->pic_slot[out] = pick;
    s->pic_serial[out] = o->damage_serial;
    s->pic_gen[out] = excluded_gen;
    /* The requests above were sent on our connection, so the server has
     * them before the SendEvent behind this FRAME, and handles them in
     * that order. */
    deliver(s, out, pick, id, 0);
}

void capture_serve(void)
{
    if (session_count == 0)
        return;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (!s->used)
            continue;
        for (int k = 0; k < s->nout; k++) {
            if (!s->pending[k])
                continue;
            CompOutput *o = (CompOutput *)output_by_id(s->out_id[k]);
            if (o)
                assemble(s, k, o);
            else {
                s->pending[k] = false;
                send_frame(s, (uint32_t)k, SLOT_NONE, s->pending_id[k], FRAME_UNAVAILABLE);
            }
        }
    }
}

static bool hides_anything(void)
{
    if (cfg.mode == MODE_HIDE)
        return true;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && sessions[i].hide_pid)
            return true;
    return false;
}
