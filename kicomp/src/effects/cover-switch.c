/*
 * cover-switch: the window list as a row of covers seen at an angle.
 *
 * The same idea Compiz's shift switcher, KWin's cover switch and iTunes'
 * album art all landed on: the windows stand in a line receding to both
 * sides, turned away from the viewer, and the one being chosen swings
 * flat to face you. Picking is a walk along the line, not a hunt across
 * a grid -- which is what makes it the right shape for Alt+Tab, where
 * the user is stepping through an order they already have in their head.
 *
 * What it shares with show-windows: a mode, a grab, an animation per
 * item, labels, a ground to sit on. What is different is the only part
 * worth writing separately -- the layout is one dimensional and the
 * transform is projective, so this is the first effect in the tree that
 * needs the perspective divide (transform.h) rather than a scale.
 *
 * Two rules it does not break:
 *
 *   Nothing is focused while the mode is up. The selection here is the
 *   effect's own; the real focus changes once, on the way out, and only
 *   if the user chose something. A switcher that focused as it went
 *   would raise and re-stack windows under the very animation drawing
 *   them, and every application would see a focus it never got to keep.
 *
 *   The keyboard is only ours while the mode runs. The intended use is
 *   Alt+Tab, where the *window manager* owns the key and the hold -- see
 *   the note on driving this from kiwm at the bottom of this file. The
 *   hotkey here is how the effect is reached without one, and how it is
 *   tested.
 */
#include "../effect.h"
#include "../window.h"
#include "../output.h"
#include "../input.h"
#include "../scene.h"
#include "../desktop.h"
#include "../transform.h"
#include "../text.h"
#include "../animation.h"
#include "../renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS 64
#define TITLE_MAX 256

typedef struct {
    char hotkey[128];

    /* How far round a cover is turned once it is fully to one side, in
     * degrees, and how strong the projection that foreshortens it is
     * (transform.h's distance, as a fraction of the output's width --
     * expressed that way so one setting looks the same on every
     * monitor). Angle 0 or perspective 0 gives a flat row, which is
     * also what the XRender backend gets. */
    float angle;
    float perspective;

    /* The front cover's size as a fraction of the output, the gap from
     * it to the first cover on each side, and the gap between the ones
     * after that -- they bunch up, so a long list still fits. All as
     * fractions of the output's width. */
    float cover;
    float gap;
    float step;

    /* How far back the turned covers sit, in root pixels. */
    float depth;

    int   visible;        /* covers drawn each side of the front one */
    float background;     /* opacity of the ground under everything */

    /* Windows from the output's other desktops as well as the one on
     * screen. They are unmapped, but the compositor keeps their last
     * contents (keep_hidden_contents, on by default), so what a cover
     * shows is the desktop as the user left it -- which for a switcher
     * is the picture that means something. Live faces, as the cube
     * holds them up for, would be a good deal more work for a mode that
     * is on screen for a second. */
    bool  other_desktops;

    /* And kept *live* while the row is up, rather than showing the
     * picture they had when their desktop was left. One row of windows
     * for a second or two is a small thing to ask the window manager to
     * hold up, and a switcher that shows a video still playing is
     * telling the truth about what it is offering.
     *
     * On kicomp's scale (comp.h's CompLiveWindows) so that, left out of
     * the section, it is kicomp's own live_windows; a row holds either
     * every window it shows or none, so `active` means the same as
     * `all` here. 0 and 1 still mean what they meant. */
    CompLiveWindows live_windows;

    /* The ground follows the selection: walking onto a window that lives
     * on another desktop fades that desktop's wallpaper in under the
     * row, rather than waiting until the row closes to show where you
     * are going. */
    bool  follow_desktop;

    bool  labels;
    /* Where the selected window's title sits, as a fraction of the
     * output's height from its top, and how wide it may grow before it
     * is wrapped. */
    float label_y;
    int   label_width;
    bool  wrap;           /* stepping past the end comes back round */
} CsConfig;

typedef struct {
    CompWindow *win;
    CompRect home;
    /* Which desktop it is on, so the ones that are not going to be on
     * screen can be seen off rather than simply cut. COMP_DESKTOP_ALL
     * for a sticky window, which is on whichever one you land on. */
    int desktop;
    char title[TITLE_MAX];
    CompTextImage *label;
} CsItem;

typedef struct {
    int output_id;
    int start_desktop;    /* the one that was showing when the row opened */

    /* Which desktop's wallpaper is the ground, and which one it is
     * crossing from. They differ only while a fade is running. */
    int ground_from, ground_to;
    double ground_time;

    CsItem items[MAX_ITEMS];
    int count;

    double held_at;       /* when the holds were last renewed */
    bool took_stowed;     /* this mode is one of the reasons the other
                           * desktops' windows are in the scene */

    int selected;         /* the item the user is on */
    float pos;            /* where the row actually is, easing to `selected` */
    double pos_time;      /* when the current glide started */
    float pos_from;

    /* How far *into* the mode the picture is: 0 leaves every window
     * exactly where it really is, untransformed; 1 is the full row.
     * Opening eases it to 1 and closing back to 0, and everything the
     * effect draws is scaled by it -- the travel, the tilt, the ground,
     * the label. That is what makes entering and leaving a movement
     * rather than a cut. */
    float phase;
    float phase_from;
    float phase_to;
    double phase_time;

    bool closing;

    /* Driven by the window manager rather than by our own hotkey: it
     * holds the keyboard, so this mode took no grab, releases none, and
     * chooses nothing -- the WM decides what the walk meant when its key
     * comes up. See the protocol at the bottom of this file. */
    bool external;
} CsData;

static const CompEffectOps cs_ops;
static CompEffect *active;

static const CompEffectInstance *bound_instance;

static void phase_to(CsData *d, float to, double now);
static void close_mode(CompEffect *e, bool activate_it);

/* ------------------------------------------------------------------ */
/* the item list                                                       */
/* ------------------------------------------------------------------ */

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(comp.conn,
        xcb_intern_atom(comp.conn, 0, (uint16_t)strlen(name), name), NULL);
    if (!r)
        return XCB_ATOM_NONE;
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void read_title(CompWindow *w, char *out, size_t outsz)
{
    out[0] = '\0';
    xcb_window_t id = w->client ? w->client : w->id;

    static xcb_atom_t net_name, utf8;
    if (!net_name) {
        net_name = atom("_NET_WM_NAME");
        utf8 = atom("UTF8_STRING");
    }

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, id, net_name, utf8, 0, 256), NULL);
    if (r && xcb_get_property_value_length(r) > 0) {
        int n = xcb_get_property_value_length(r);
        if (n > (int)outsz - 1)
            n = (int)outsz - 1;
        memcpy(out, xcb_get_property_value(r), (size_t)n);
        out[n] = '\0';
    }
    free(r);

    if (out[0])
        return;

    r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, id, XCB_ATOM_WM_NAME,
                         XCB_ATOM_STRING, 0, 256), NULL);
    if (r && xcb_get_property_value_length(r) > 0) {
        int n = xcb_get_property_value_length(r);
        if (n > (int)outsz - 1)
            n = (int)outsz - 1;
        memcpy(out, xcb_get_property_value(r), (size_t)n);
        out[n] = '\0';
    }
    free(r);
}

/* Which desktop a window is on, or COMP_DESKTOP_ALL when it is on all
 * of them. -1 when there is no answer. */
static int desktop_of(const CompWindow *w)
{
    int desktop = 0, index = 0;
    if (!desktop_of_window(w, &desktop, &index))
        return -1;
    return desktop;
}

static CompOutput *output_by_id(int id)
{
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == id)
            return &comp.outputs[i];
    return NULL;
}

static CompOutput *output_of(const CompRect *r)
{
    CompOutput *best = NULL;
    long best_area = 0;
    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (!rect_intersect(r, &comp.outputs[i].rect, &hit))
            continue;
        long a = (long)hit.w * hit.h;
        if (a > best_area) {
            best_area = a;
            best = &comp.outputs[i];
        }
    }
    return best;
}

/* Whether a window that is not on screen has a place in the row.
 *
 * Minimized, with the picture it had when it went (comp.h's stowed): yes,
 * always -- it is one of this desktop's windows, put away, and a switcher
 * that cannot reach it is a switcher that cannot reach half of what is
 * open. Away with one of this output's other desktops: with other_desktops
 * on, from its picture -- or, where there is no picture at all, because
 * it was put away before this compositor was running, with an empty
 * cover this once and a hold asked for it (hold_live_windows), so there
 * is a picture by the next time. A minimized window with no picture is
 * left out: nothing can be asked for it. */
static bool away_but_showable(const CompWindow *w, const CsConfig *cfg,
                              int output_id)
{
    if (w->state & COMP_STATE_MINIMIZED)
        return w->stowed;
    if (!cfg->other_desktops)
        return false;
    if (w->stowed)
        return true;
    /* No picture: only if it really is away with another desktop. An
     * unmapped window *on* this desktop is one that has not opened yet,
     * or one its owner hid, and neither is a cover. */
    int desk = desktop_of(w);
    return desk >= 0 && desk != desktop_current_for_output(output_by_id(output_id));
}

static bool eligible(const CompWindow *w, const CompEffectInstance *self, int output_id)
{
    const CsConfig *cfg = self->config;

    if (w->input_only || w->zombie || w->wm_layer[0])
        return false;

    /* On screen, or put away with something to show for it
     * (away_but_showable). */
    if (!w->mapped && !away_but_showable(w, cfg, output_id))
        return false;
    if (w->type == COMP_WINDOW_DOCK || w->type == COMP_WINDOW_DESKTOP)
        return false;
    if (!(self->windows & COMP_WINDOW_BIT(w->type)))
        return false;

    CompRect r = window_rect(w);
    if (r.w <= 0 || r.h <= 0)
        return false;

    CompOutput *o = output_of(&r);
    return o && o->id == output_id;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

/* How solid a cover is, by how far out it has travelled.
 *
 * One all the way out to the last cover the row shows, then down to
 * nothing across the width of one more. So exactly one cover at each end
 * is ever part-way, which is what makes a long list read as a row that
 * continues past the edge rather than one that stops dead there. */
static float edge_alpha(const CsConfig *cfg, float slot)
{
    float d = fabsf(slot);
    float last = (float)cfg->visible;
    if (d <= last)
        return 1.0f;
    if (d >= last + 1.0f)
        return 0.0f;
    return 1.0f - (d - last);
}

/* Where item `i` sits, as a signed distance from the front of the row.
 * Fractional while the row is gliding, which is what makes the covers
 * turn *through* the movement rather than snap at the end of it. */
static float slot_of(const CsData *d, int i)
{
    return (float)i - d->pos;
}

/* The front cover's rectangle on this output: the window scaled to fit
 * the configured box, centred. Its own aspect is kept -- a cover that
 * stretched its window would be showing something the user has never
 * seen. */
static CompRect cover_rect(const CsItem *it, const CompOutput *o, const CsConfig *cfg)
{
    float box_w = (float)o->rect.w * cfg->cover;
    float box_h = (float)o->rect.h * cfg->cover;

    float sw = box_w / (float)it->home.w;
    float sh = box_h / (float)it->home.h;
    float s = sw < sh ? sw : sh;
    if (s > 1.0f)
        s = 1.0f;              /* never blow a small window up past life size */

    int w = (int)((float)it->home.w * s + 0.5f);
    int h = (int)((float)it->home.h * s + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    return (CompRect){ o->rect.x + (o->rect.w - w) / 2,
                       o->rect.y + (o->rect.h - h) / 2, w, h };
}

/* The transform that takes a cover from the middle of the row to its
 * place in it.
 *
 * Built in the order the reading goes: move the cover's centre to the
 * origin, turn it, push it back, slide it along, project, and put the
 * origin back in the middle of the output. Every step after the turn
 * happens in the turned frame, which is what gives the near edge of a
 * side cover its overhang. */
static void cover_transform(CompTransform *t, const CompRect *geo,
                            const CompOutput *o, const CsConfig *cfg,
                            float slot, float phase)
{
    float clamped = slot < -1.0f ? -1.0f : (slot > 1.0f ? 1.0f : slot);

    /* Turned in proportion to how far off centre it is, so the front
     * cover is flat and the turn completes exactly as it leaves -- and
     * in proportion to the phase, so the whole row tilts up out of the
     * desktop when the mode opens and lies back down when it closes. */
    float angle = -cfg->angle * (float)M_PI / 180.0f * clamped * phase;

    /* Slid: the first cover each side is a gap away, the ones past it a
     * smaller step each, so a long list crowds towards the edges instead
     * of marching off the screen. */
    float extra = slot - clamped;
    float x = (float)o->rect.w * (cfg->gap * clamped + cfg->step * extra) * phase;

    /* Pushed back once it is fully turned. */
    float z = -cfg->depth * fabsf(clamped) * phase;

    /* About the rectangle the node is actually being drawn in, and back
     * to it afterwards -- not to the middle of the output. At phase 0
     * the geometry is the window's own rectangle and all three terms
     * above are zero, so what comes out is the identity and the window
     * is drawn exactly where it is. Everything between is a real
     * position, which is the whole of "it moves rather than jumps". */
    float cx = (float)geo->x + (float)geo->w * 0.5f;
    float cy = (float)geo->y + (float)geo->h * 0.5f;

    comp_transform_identity(t);
    comp_transform_translate(t, -cx, -cy);
    comp_transform_rotate_y(t, angle);
    comp_transform_translate_z(t, z);
    comp_transform_translate(t, x, 0.0f);
    comp_transform_perspective(t, (float)o->rect.w * cfg->perspective);
    comp_transform_translate(t, cx, cy);
}

/* Where the node sits on its way from the window's own rectangle to its
 * cover: the two interpolated by the phase. */
static CompRect placed_rect(const CsItem *it, const CompOutput *o,
                            const CsConfig *cfg, float phase)
{
    CompRect cover = cover_rect(it, o, cfg);
    if (phase >= 1.0f)
        return cover;
    if (phase <= 0.0f)
        return it->home;

    return (CompRect){
        it->home.x + (int)((float)(cover.x - it->home.x) * phase),
        it->home.y + (int)((float)(cover.y - it->home.y) * phase),
        it->home.w + (int)((float)(cover.w - it->home.w) * phase),
        it->home.h + (int)((float)(cover.h - it->home.h) * phase),
    };
}

/* ------------------------------------------------------------------ */
/* the mode                                                            */
/* ------------------------------------------------------------------ */

static void mark_dirty(const CsData *d)
{
    (void)d;
    /* The whole screen: a row of covers in perspective overlaps itself
     * and the ground under it, and working out the union of where every
     * cover was and now is would cost more than the repaint saves for
     * the fraction of a second this mode is up. */
    output_damage_all();
}

/* Drop a window the compositor is forgetting (effect.h). The row goes on
 * without it rather than closing: a window closing itself while the user
 * is walking the list is an ordinary thing, and throwing them out of the
 * switcher for it would be the surprising answer. */
static void cs_window_gone(CompEffect *e, CompWindow *w)
{
    CsData *d = e->data;
    if (!d)
        return;

    for (int i = 0; i < d->count; i++) {
        if (d->items[i].win != w)
            continue;

        if (d->items[i].label)
            text_free(d->items[i].label);

        memmove(&d->items[i], &d->items[i + 1],
                sizeof(CsItem) * (size_t)(d->count - i - 1));
        d->count--;

        /* Keep the selection on the window it was on. Removing something
         * ahead of it in the row shifts everything after it down one, so
         * the index has to come down with it -- and the row's position
         * with the index, or the covers would slide by one for a change
         * the user did not make. */
        if (i < d->selected) {
            d->selected--;
            d->pos -= 1.0f;
            d->pos_from -= 1.0f;
        } else if (i == d->selected && d->selected >= d->count) {
            d->selected = d->count - 1;
        }

        if (d->count == 0 && !d->closing)
            close_mode(e, false);
        else
            mark_dirty(d);
        return;
    }
}

/* Windows that appeared since the row was built, appended to the end.
 *
 * The end rather than the top of the stack, which is where a new window
 * really is: the user is part-way through a walk along this row, and
 * inserting ahead of where they are would move the thing under their
 * finger. Cheap enough to ask every frame -- it is a pointer comparison
 * per window per item, over the handful of each that a screen has. */
/* Asks the window manager to keep the row's away-with-their-desktop
 * windows on screen, renewed because kiwm caps a hold at two seconds of
 * its own accord and a hold-down Alt+Tab can outlast that.
 *
 * Only the ones that are actually away: everything on this desktop is
 * live for free, and asking about it would be asking the WM to hold up
 * something it is already showing. */
#define HOLD_MS       1500
#define HOLD_RENEW_MS 500

static void hold_live_windows(CompEffect *e, double now)
{
    CsData *d = e->data;
    const CsConfig *cfg = e->instance->config;

    if (!cfg->other_desktops || d->closing)
        return;
    if (d->held_at != 0.0 && now - d->held_at < HOLD_RENEW_MS)
        return;
    d->held_at = now;

    /* The other desktops' own wallpapers, renewed while the row is up.
     *
     * Not for the row itself, which shows windows and not grounds, but
     * for what happens straight after it: choosing a window on another
     * desktop hands over to the desktop wall, and the wall slides that
     * desktop in with whatever picture of its wallpaper the compositor
     * has. With none -- and there is none until something draws it once
     * while the window manager has it up -- the desktop arrives on
     * black. Asking here means the photograph is taken during the
     * second the user spends choosing. */
    desktop_request_prime();

    /* The windows: every one away with its desktop where live_windows
     * (this section's, or kicomp's own) says so -- and whatever it says,
     * one there is no picture of at all, held up once so that it is
     * drawn here, which names its pixmap, and kept when the hold ends
     * (away_but_showable). Minimized windows are never asked for: kiwm
     * refuses, and they are drawn from their pictures. */
    bool live = comp_live_windows_resolve(cfg->live_windows) != COMP_LIVE_DESKTOP;

    for (int i = 0; i < d->count; i++) {
        CompWindow *w = d->items[i].win;
        if (!w || w->mapped || (w->state & COMP_STATE_MINIMIZED))
            continue;
        if (live || !renderer_window_has_content(w))
            desktop_request_hold(w, HOLD_MS);
    }
}

static void refresh_items(CompEffect *e)
{
    CsData *d = e->data;
    const CsConfig *cfg = e->instance->config;
    if (d->closing || d->count >= MAX_ITEMS)
        return;

    /* Not when a window manager is driving: the list it gave is the
     * list, in the order it means to walk, and adding to it here would
     * put covers in the row that its key never reaches. If the set
     * changes it writes the property again. */
    if (d->external)
        return;

    bool added = false;
    for (CompWindow *w = comp.stack; w && d->count < MAX_ITEMS; w = w->next) {
        if (!eligible(w, e->instance, d->output_id))
            continue;

        bool known = false;
        for (int i = 0; i < d->count && !known; i++)
            known = d->items[i].win == w;
        if (known)
            continue;

        CsItem *it = &d->items[d->count++];
        memset(it, 0, sizeof(*it));
        it->win = w;
        it->home = window_rect(w);
        it->desktop = desktop_of(w);
        read_title(w, it->title, sizeof(it->title));
        if (cfg->labels)
            it->label = text_render(it->title, text_theme_style(), cfg->label_width);
        added = true;
    }

    if (added)
        mark_dirty(d);
}

static void step_selection(CompEffect *e, int by)
{
    CsData *d = e->data;
    const CsConfig *cfg = e->instance->config;
    if (d->count == 0)
        return;

    int next = d->selected + by;
    if (cfg->wrap) {
        next = (next % d->count + d->count) % d->count;
    } else {
        if (next < 0) next = 0;
        if (next >= d->count) next = d->count - 1;
    }
    if (next == d->selected)
        return;

    /* The row glides from wherever it actually is, not from the slot it
     * was last told to be at: holding Tab down is a stream of steps, and
     * each one should continue the movement rather than restart it. */
    d->pos_from = d->pos;
    d->pos_time = comp_now_ms();
    d->selected = next;
    mark_dirty(d);
}

/* Ask the window manager to focus this one. A compositor does not move
 * the focus itself -- it has no business deciding what is active, and
 * the WM is the only thing that can raise and re-stack correctly. So
 * this is the ordinary _NET_ACTIVE_WINDOW request any pager sends, and
 * what the WM does with it is the WM's affair. */
static void activate(CompWindow *w)
{
    static xcb_atom_t net_active;
    if (!net_active)
        net_active = atom("_NET_ACTIVE_WINDOW");
    if (net_active == XCB_NONE)
        return;

    xcb_client_message_event_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = w->client != XCB_NONE ? w->client : w->id;
    msg.type = net_active;
    msg.data.data32[0] = 2;              /* a pager, not the app itself */
    msg.data.data32[1] = XCB_CURRENT_TIME;

    xcb_send_event(comp.conn, 0, comp.root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&msg);
    xcb_flush(comp.conn);
}

static void close_mode(CompEffect *e, bool activate_it)
{
    CsData *d = e->data;
    if (d->closing)
        return;

    if (!d->external)
        input_release();

    if (activate_it && d->selected >= 0 && d->selected < d->count) {
        CompWindow *w = d->items[d->selected].win;
        if (w && !w->zombie) {
            double cover = comp.claim_ms;

            /* The user picked this window out of a row of them: they
             * looked at the lot and pointed. Dodge answering to the
             * focus that follows would rearrange the desktop underneath
             * their own decision -- which is the reasoning dodge already
             * applies to show-windows, through input_mode_ended_ms().
             * That one cannot help here: driven from the window manager
             * this mode never takes a grab, so there is no mode ending
             * for dodge to notice. */
            effects_claim_window(COMP_EVENT_FOCUS, w, cover);

            /* If the window is on another desktop, activating it takes
             * the desktop with it -- and this row is what shows that
             * happening: the covers fly home and the two wallpapers
             * cross. Nothing else animates the same change on top of
             * it, the wall least of all, which would be a second answer
             * to one question and would break the rule that one thing
             * at a time takes the screen.
             *
             * Claimed for the whole output rather than for this window
             * alone. A wall that ran for every window *except* the
             * chosen one would be exactly the half-animation this is
             * meant to avoid. */
            effects_claim(COMP_EVENT_DESKTOP_ENTER, d->output_id, cover);
            effects_claim(COMP_EVENT_DESKTOP_LEAVE, d->output_id, cover);

            /* The claims are made either way; the activation only when
             * this mode is its own. Driven from the window manager it is
             * the WM that focuses -- but the animation is still ours, and
             * so is knowing which window must not be animated. */
            if (!d->external)
                activate(w);
        }
    }

    d->closing = true;
    phase_to(d, 0.0f, comp_now_ms());
    active = NULL;
    mark_dirty(d);
}

static bool on_key(void *data, xcb_keysym_t sym, const char *text, uint16_t mods)
{
    CompEffect *e = data;
    (void)text;
    (void)mods;

    switch (sym) {
    case 0xff09:                  /* Tab */
        step_selection(e, (mods & XCB_MOD_MASK_SHIFT) ? -1 : +1);
        return true;
    case 0xff51:                  /* Left */
        step_selection(e, -1);
        return true;
    case 0xff53:                  /* Right */
        step_selection(e, +1);
        return true;
    case 0xff0d:                  /* Return */
    case 0xff8d:                  /* KP_Enter */
    case 0x0020:                  /* space */
        close_mode(e, true);
        return true;
    case 0xff1b:                  /* Escape */
        close_mode(e, false);
        return true;
    default:
        return false;
    }
}

/* Which cover is under this point, or -1.
 *
 * By the bounding box of where each one is actually drawn, and when two
 * overlap the one nearer the front wins -- which is both what the eye
 * reads as "on top" (they are drawn in that order) and what the hand
 * means, since the front cover is the one not hidden behind anything. */
static int item_at(const CsData *d, const CompOutput *o, const CsConfig *cfg,
                   int x, int y)
{
    int best = -1;
    float best_slot = 0.0f;

    for (int i = 0; i < d->count; i++) {
        float slot = slot_of(d, i);
        if (fabsf(slot) > (float)cfg->visible + 1.0f)
            continue;

        CompRect geo = placed_rect(&d->items[i], o, cfg, d->phase);
        CompTransform t;
        cover_transform(&t, &geo, o, cfg, slot, d->phase);

        CompRect box;
        comp_transform_bbox(&t, &geo, &box);
        if (x < box.x || y < box.y || x >= box.x + box.w || y >= box.y + box.h)
            continue;

        if (best < 0 || fabsf(slot) < best_slot) {
            best = i;
            best_slot = fabsf(slot);
        }
    }
    return best;
}

static void on_button(void *data, int root_x, int root_y, uint8_t button, bool pressed)
{
    CompEffect *e = data;
    CsData *d = e->data;
    const CsConfig *cfg = e->instance->config;

    /* On the release, not the press: a press is a user still deciding --
     * they can slide off what they pressed on and let go somewhere else,
     * the way every button on every desktop works. The wheel has no
     * release worth waiting for. */
    if (button == 4 || button == 5) {
        if (pressed)
            step_selection(e, button == 4 ? -1 : +1);
        return;
    }
    if (pressed)
        return;

    if (button == 3) {
        close_mode(e, false);
        return;
    }
    if (button != 1)
        return;

    CompOutput *o = output_by_id(d->output_id);
    int hit = o ? item_at(d, o, cfg, root_x, root_y) : -1;

    if (hit < 0) {
        /* The ground around the row: a click there is a click on nothing,
         * which everywhere else means "never mind". */
        close_mode(e, false);
        return;
    }

    /* Clicking a cover is choosing it, exactly as Return chooses the one
     * in front. Selected first so the walk out starts from the right
     * place -- the row is still a row while it lies back down. */
    d->pos_from = d->pos;
    d->pos_time = comp_now_ms();
    d->selected = hit;
    close_mode(e, true);
}

static const CompInputHandler cs_input = {
    .key    = on_key,
    .button = on_button,
};

/* ------------------------------------------------------------------ */
/* animation                                                           */
/* ------------------------------------------------------------------ */

/* Eased progress of a leg that started at `since`. Two of them run
 * independently: the phase (into and out of the mode) and the row's
 * glide between covers, because a step taken while the mode is still
 * opening should continue into it rather than wait for it. */
static float eased(const CompEffect *e, double since, double now)
{
    double len = effect_instance_duration(e->instance);
    if (len <= 0.0)
        len = 1.0;
    double t = (now - since) / len;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return comp_ease(e->instance->easing, (float)t);
}

static void phase_to(CsData *d, float to, double now)
{
    d->phase_from = d->phase;
    d->phase_to = to;
    d->phase_time = now;
}

/* The ground the row is standing on, kept in step with the selection.
 *
 * Asked once a frame rather than at each of the places the selection can
 * change -- a key, the wheel, a click, a window manager writing the
 * property -- because there are four of those and one of this. */
static void ground_follow(CompEffect *e, double now)
{
    CsData *d = e->data;
    const CsConfig *cfg = e->instance->config;

    if (!cfg->follow_desktop || d->closing)
        return;
    if (d->selected < 0 || d->selected >= d->count)
        return;

    int want = d->items[d->selected].desktop;
    if (want < 0 || want == COMP_DESKTOP_ALL)
        return;                     /* sticky: it is on this one too */
    if (want == d->ground_to)
        return;

    d->ground_from = d->ground_to;
    d->ground_to = want;
    d->ground_time = now;
    mark_dirty(d);
}

static void cs_update(CompEffect *e, double now)
{
    CsData *d = e->data;

    hold_live_windows(e, now);
    ground_follow(e, now);
    refresh_items(e);

    float pp = eased(e, d->phase_time, now);
    float phase = d->phase_from + (d->phase_to - d->phase_from) * pp;

    float gp = eased(e, d->pos_time, now);
    float glide = d->pos_from + ((float)d->selected - d->pos_from) * gp;

    if (phase != d->phase || glide != d->pos) {
        d->phase = phase;
        d->pos = glide;
        mark_dirty(d);
    }
}

/* Done only once the windows are home again: the mode ends when the
 * picture has finished leaving, not when the key was let go. */
static bool cs_finished(const CompEffect *e, double now)
{
    const CsData *d = e->data;
    (void)now;
    return d->closing && d->phase <= 0.0f;
}

/* While the mode is up a window is drawn somewhere other than where it
 * is, so its own damage has to be carried there (effect.h). */
static bool cs_damage_map(const CompEffect *e, const CompWindow *w,
                          const CompRect *in, CompRect *out)
{
    const CsData *d = e->data;
    CompOutput *o = output_by_id(d->output_id);
    const CsConfig *cfg = e->instance->config;
    if (!o)
        return false;

    for (int i = 0; i < d->count; i++) {
        if (d->items[i].win != w)
            continue;

        CompRect cover = placed_rect(&d->items[i], o, cfg, d->phase);
        CompTransform t;
        cover_transform(&t, &cover, o, cfg, slot_of(d, i), d->phase);

        /* The damage arrived in the window's own coordinates; the cover
         * is the window scaled into `cover`, so the rectangle is carried
         * through that scaling before the row's transform. */
        float sx = (float)cover.w / (float)d->items[i].home.w;
        float sy = (float)cover.h / (float)d->items[i].home.h;
        CompRect scaled = {
            cover.x + (int)((float)(in->x - d->items[i].home.x) * sx),
            cover.y + (int)((float)(in->y - d->items[i].home.y) * sy),
            (int)((float)in->w * sx + 1.0f),
            (int)((float)in->h * sy + 1.0f),
        };
        comp_transform_bbox(&t, &scaled, out);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static int index_of(const CsData *d, const CompWindow *w)
{
    for (int i = 0; i < d->count; i++)
        if (d->items[i].win == w)
            return i;
    return -1;
}

static void cs_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    CsData *d = e->data;
    const CsConfig *cfg = e->instance->config;
    if (o->id != d->output_id)
        return;

    float alive = d->phase;

    if (cfg->background > 0.0f)
        scene_set_backdrop(s, &o->rect, 0.0f, 0.0f, 0.0f,
                           cfg->background * alive);

    bool done[MAX_ITEMS];
    memset(done, 0, sizeof(done));

    /* Which items belong to a desktop that is about to stop showing:
     * everything whose desktop is neither the chosen window's nor "all".
     * Worked out once rather than per node, since the answer is the same
     * for the whole frame. */
    bool leaving[MAX_ITEMS];
    memset(leaving, 0, sizeof(leaving));
    if (d->closing && d->selected >= 0 && d->selected < d->count) {
        int landing = d->items[d->selected].desktop;
        for (int i = 0; i < d->count; i++)
            leaving[i] = d->items[i].desktop >= 0 &&
                         d->items[i].desktop != landing &&
                         d->items[i].desktop != COMP_DESKTOP_ALL;
    }

    /* Two passes over the scene: place every cover, then put the nodes
     * in back-to-front order. There is no depth buffer -- the renderers
     * draw the list in order -- so the order *is* the depth, and a row
     * drawn in stacking order would put the far covers over the near
     * ones. */
    for (int n = 0; n < s->count; n++) {
        CompSceneNode *node = &s->nodes[n];
        int i = index_of(d, node->win);

        if (i < 0) {
            /* Not in the row: the wallpaper, a panel, or a window that
             * appeared after the mode opened.
             *
             * The ones belonging to another desktop are not drawn at
             * all. With other_desktops on they are in the scene -- that
             * is what lets the row show their windows -- and left alone
             * every desktop's wallpaper is painted over every other,
             * which comes out as all of them at once through the
             * dimming. Only the desktop the row opened on is the ground
             * here; the rest are in it for their windows, not for their
             * scenery. */
            int nd = desktop_of(node->win);

            /* Sticky, or not on a desktop at all: it is the ground of
             * whichever desktop you are on, and never fades. */
            if (nd < 0 || nd == COMP_DESKTOP_ALL) {
                node->opacity *= 1.0f - cfg->background * alive;
                continue;
            }

            /* Two grounds at most: the one the row is standing on and
             * the one it is crossing to. Anything else belongs to a
             * desktop nobody is looking at and is not drawn -- with
             * other_desktops on they are all in the scene, and left
             * alone every wallpaper is painted over every other, which
             * through the dimming comes out as all of them at once. */
            float mix = eased(e, d->ground_time, comp_now_ms());
            float ground;

            if (nd == d->ground_to)
                ground = (d->ground_from == d->ground_to) ? 1.0f : mix;
            else if (nd == d->ground_from)
                ground = 1.0f - mix;
            else
                ground = 0.0f;

            /* The ones that are not the ground are still *drawn*, at
             * nothing.
             *
             * A window's pixmap is only named when something draws it,
             * and a desktop's wallpaper is only up for the 700 ms the
             * window manager raises it for. Hiding the other desktops'
             * grounds outright meant the one the user walks towards had
             * never been photographed, and the fade arrived at black.
             * Drawn at zero they are photographed while the user is
             * choosing and cannot be seen doing it -- which is also why
             * this is opacity and not stacking: nothing can leak through
             * from behind if nothing behind is opaque.
             *
             * It costs a full-screen quad per desktop per frame for the
             * second or two the row is up, and buys the fade having
             * something to fade to. */
            node->opacity *= (1.0f - cfg->background * alive) * ground;
            continue;
        }

        float slot = slot_of(d, i);
        if (fabsf(slot) > (float)cfg->visible + 1.0f) {
            node->visible_rect = (CompRect){ 0, 0, 0, 0 };
            continue;
        }

        CompRect geo = placed_rect(&d->items[i], o, cfg, d->phase);
        CompTransform t;
        cover_transform(&t, &geo, o, cfg, slot, d->phase);

        /* The geometry travels from the window's own rectangle to its
         * cover; the transform turns it in place about wherever it has
         * got to. Together that is a window that visibly flies into the
         * row and tilts as it goes. */
        node->geometry = geo;
        node->transform = t;
        comp_transform_bbox(&t, &geo, &node->visible_rect);

        /* A cover is never dimmed for being unselected -- every window in
         * the row is a real window and the user is reading them, not
         * being told which one is chosen; the one in front is already
         * marked out by facing them. The only opacity in the row is at
         * its two ends, where the outermost cover fades: with more
         * windows than the row shows, that is what lets one travel off
         * one end while another arrives at the other instead of both
         * appearing and vanishing outright.
         *
         * And nothing fades on the way in or out. At phase 0 a window
         * has to look exactly as it does on the desktop, because that
         * is where it still is -- the movement is the whole effect and
         * a fade would hide it. */
        node->opacity *= edge_alpha(cfg, slot);

        /* On the way out, the windows that are not going to be on screen
         * are seen off rather than simply cut.
         *
         * The row can hold windows from several desktops (other_desktops);
         * when it closes, only the chosen window's desktop stays. Letting
         * the rest simply stop being drawn at the end of the animation is
         * a row that half vanishes, so they fade as they fly home --
         * which is also the truth about them: they are going away.
         *
         * A sticky window is on whichever desktop you land on, so it
         * never fades. */
        if (d->closing && leaving[i])
            node->opacity *= alive;
    }

    /* Back to front, and the whole row above everything else.
     *
     * There is no depth buffer -- the renderers draw the list in order
     * and that order is the depth -- so each cover is moved to the end
     * of the list, furthest from the middle first. The one facing the
     * user is moved last and therefore ends up on top of every other
     * cover, which is what "the one you are choosing is in front" has
     * to mean when the covers overlap.
     *
     * Moving to the end rather than sorting in place is the point: an
     * in-place sort that only ever swaps neighbours cannot move a cover
     * past a node that is not one, so a dock or a window that opened
     * mid-mode sitting between two covers would pin them where they
     * were. This way the row ends up above those too, which is also
     * where a mode belongs. */
    for (int pass = 0; pass < d->count; pass++) {
        int pick = -1;
        float pick_slot = -1.0f;

        for (int a = 0; a < s->count; a++) {
            int i = index_of(d, s->nodes[a].win);
            if (i < 0 || done[i])
                continue;
            float dist = fabsf(slot_of(d, i));
            if (pick < 0 || dist > pick_slot) {
                pick = a;
                pick_slot = dist;
            }
        }
        if (pick < 0)
            break;

        done[index_of(d, s->nodes[pick].win)] = true;
        scene_move_node(s, pick, s->count - 1);
    }

    /* The title of the one in front, under the row. */
    if (cfg->labels && d->selected >= 0 && d->selected < d->count) {
        CompTextImage *img = d->items[d->selected].label;
        if (img) {
            int tw = text_width(img), th = text_height(img);
            CompRect where = {
                o->rect.x + (o->rect.w - tw) / 2,
                o->rect.y + (int)((float)o->rect.h * cfg->label_y) - th / 2,
                tw, th
            };
            scene_add_chrome(s, img, &where, alive);
        }
    }
}

static void cs_destroy(CompEffect *e)
{
    CsData *d = e->data;
    if (!d)
        return;

    /* The other desktops go back to being invisible the moment this
     * stops drawing them -- and only if this mode was one of the reasons
     * they were visible, since the count is shared. */
    if (d->took_stowed)
        effects_show_stowed(d->output_id, false);
    for (int i = 0; i < d->count; i++)
        if (d->items[i].label)
            text_free(d->items[i].label);
    free(d);
    e->data = NULL;
}

/* The row is a desktop-scale mode: while it is up nothing else that
 * takes the screen over may start (effect.h). It adjusts the scene
 * rather than replacing it, so it does not draw alone. */
static int cs_owns_output(const CompEffect *e)
{
    const CsData *d = e->data;
    return d ? d->output_id : COMP_NO_OUTPUT;
}

static const CompEffectOps cs_ops = {
    .name       = "cover-switch",
    .owns_output = cs_owns_output,
    .update     = cs_update,
    .apply      = cs_apply,
    .finished   = cs_finished,
    .window_gone = cs_window_gone,
    .destroy    = cs_destroy,
    .damage_map = cs_damage_map,
};

/* ------------------------------------------------------------------ */
/* the trigger                                                         */
/* ------------------------------------------------------------------ */

/* Starts the mode on `o` with the windows `wins` names, or with every
 * eligible window on that output when `wins` is NULL (the hotkey's
 * case). Returns the effect, or NULL when there is nothing to show.
 *
 * Takes no grab: the caller decides whether this mode is driven by our
 * own keyboard handler or by a window manager that already has one. */
static CompEffect *open_mode(const CompEffectInstance *self, CompOutput *o,
                             const xcb_window_t *wins, int count, int selected)
{
    const CsConfig *cfg = self->config;

    /* One thing at a time takes the screen over: a cube, an expo grid, a
     * row of covers, a wall. Two of those at once is not a picture of
     * anything, so this simply does not open and the key does nothing
     * (effect.h). */
    if (effects_mode_running(o->id, &cs_ops))
        return NULL;

    CompEffect *e = calloc(1, sizeof(*e));
    CsData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return NULL;
    }

    /* Asked afresh rather than taken from what was last seen. Which
     * desktop an output is on reaches the compositor as a property
     * change, and a mode opened in the moment after a switch -- the
     * cube's own, or a switcher's -- would otherwise lay itself out
     * against the desktop that has just been left. */
    desktop_refresh();

    d->output_id = o->id;
    d->start_desktop = desktop_current_for_output(o);
    d->ground_from = d->ground_to = d->start_desktop;

    if (wins) {
        /* The window manager's own list and the window manager's own
         * order: it is the one walking it, and a switcher that showed a
         * different order than the key steps through would be lying. A
         * name we do not know is skipped rather than refused -- it may
         * be a window on another output, or one that closed between the
         * WM writing the list and us reading it. */
        for (int i = 0; i < count && d->count < MAX_ITEMS; i++) {
            CompWindow *w = window_find_by_client(wins[i]);
            if (!w)
                w = window_find(wins[i]);
            if (!w || w->zombie)
                continue;
            /* Not `mapped`: a window away with one of this output's other
             * desktops is unmapped, and it is exactly the one the window
             * manager is asking us to show. What it needs instead is a
             * picture -- on screen now, or kept from when it left. */
            if (!w->mapped && !away_but_showable(w, cfg, d->output_id))
                continue;

            CsItem *it = &d->items[d->count++];
            it->win = w;
            it->home = window_rect(w);
            it->desktop = desktop_of(w);
            read_title(w, it->title, sizeof(it->title));
            if (cfg->labels)
                it->label = text_render(it->title, text_theme_style(),
                                        cfg->label_width);
        }
    } else {
        /* Top of the stack first: the row reads the way the user's own
         * most-recent order does, which is the order Alt+Tab walks. */
        for (CompWindow *w = comp.stack; w && d->count < MAX_ITEMS; w = w->next) {
            if (!eligible(w, self, d->output_id))
                continue;
            CsItem *it = &d->items[d->count++];
            it->win = w;
            it->home = window_rect(w);
            it->desktop = desktop_of(w);
            read_title(w, it->title, sizeof(it->title));
            if (cfg->labels)
                it->label = text_render(it->title, text_theme_style(),
                                        cfg->label_width);
        }

        /* comp.stack runs bottom to top, so the list came out that way:
         * reverse it. */
        for (int i = 0, j = d->count - 1; i < j; i++, j--) {
            CsItem tmp = d->items[i];
            d->items[i] = d->items[j];
            d->items[j] = tmp;
        }
    }

    if (d->count < 2) {
        for (int i = 0; i < d->count; i++)
            if (d->items[i].label)
                text_free(d->items[i].label);
        free(d);
        free(e);
        return NULL;
    }

    if (selected < 0)
        selected = 0;
    if (selected >= d->count)
        selected = d->count - 1;

    /* And say that this output is the one showing them. A window kept
     * only because its contents are worth keeping is deliberately left
     * out of the scene, or it would appear on a desktop it is not on
     * (scene.c); an effect that means to draw those has to say so. */
    if (cfg->other_desktops) {
        effects_show_stowed(o->id, true);
        d->took_stowed = true;
    }

    e->ops = &cs_ops;
    e->instance = self;
    e->window = NULL;
    e->start_time = comp_now_ms();
    e->duration = 0.0;            /* finished() decides */
    e->data = d;

    d->selected = selected;
    d->pos = 0.0f;
    d->pos_from = 0.0f;
    d->pos_time = comp_now_ms();
    d->phase = 0.0f;              /* every window still exactly where it is */
    phase_to(d, 1.0f, comp_now_ms());

    return e;
}

static void cs_toggle(void *data)
{
    const CompEffectInstance *self = data;

    if (active) {
        step_selection(active, +1);   /* the hotkey again walks the row */
        return;
    }

    /* The active screen is the one the pointer is on, decided once. */
    int px = 0, py = 0;
    input_pointer_position(&px, &py);
    CompRect at = { px, py, 1, 1 };
    CompOutput *o = output_of(&at);
    if (!o)
        o = comp.output_count > 0 ? &comp.outputs[0] : NULL;
    if (!o)
        return;

    /* One in, so the row opens on the window behind the active one --
     * which is what a switcher is for. */
    CompEffect *e = open_mode(self, o, NULL, 0, 1);
    if (!e)
        return;

    if (!input_grab(&cs_input, e)) {
        cs_destroy(e);
        free(e);
        return;
    }

    active = e;
    effects_add(e);
    mark_dirty(e->data);
}

/* ------------------------------------------------------------------ */
/* driven by the window manager                                        */
/* ------------------------------------------------------------------ */

/* Only one client can hold the keyboard, and the one that must hold it
 * for Alt+Tab is the window manager: it owns the key, it owns the hold,
 * and it is the only thing that can decide and carry out what the walk
 * meant. So the compositor never grabs for this. The WM writes what it
 * wants shown on the root window and the compositor draws it -- and if
 * the compositor is not running, or was built without this effect, or
 * has it switched off, the WM sees no answer and falls back to its own
 * on-screen display with nothing having been negotiated.
 *
 *   _KICOMP_EFFECTS   on the window owning _NET_WM_CM_Sn: the names of
 *                     the modes that can be driven this way, space
 *                     separated. Its absence is the whole of "do not
 *                     try" -- a WM reads it once, when it needs to
 *                     decide which switcher to use.
 *
 *   _KICOMP_SWITCHER  on the root, CARDINAL/32:
 *                       [0] state: 0 end, 1 show, 2 end (chosen)
 *                       [1] the selected entry's index
 *                       [2..] the windows, in the order to show them
 *                     Written on every step of the walk; the compositor
 *                     watches it and moves the row to match.
 *
 * States 0 and 2 differ only in what the WM does next -- this side
 * closes the same way either time, and focuses nothing at all, because
 * the WM is the one that knows whether the user let go or gave up. */
void cover_switch_external(const uint32_t *data, int len)
{
    if (!bound_instance || len < 2)
        return;

    uint32_t state = data[0];
    int selected = (int)data[1];

    if (state != 1) {
        /* State 2 is the window manager saying the user chose the
         * selection rather than gave up. Nothing here focuses it -- the
         * WM does that -- but the difference decides whether the window
         * they chose is exempt from whatever else animates the change
         * (effect.h's effects_claim_window). */
        if (active && ((CsData *)active->data)->external)
            close_mode(active, state == 2);
        return;
    }

    if (active) {
        CsData *d = active->data;
        if (!d->external)
            return;            /* our own hotkey has it; leave it alone */
        if (selected != d->selected) {
            d->pos_from = d->pos;
            d->pos_time = comp_now_ms();
            d->selected = selected < 0 ? 0
                        : (selected >= d->count ? d->count - 1 : selected);
            mark_dirty(d);
        }
        return;
    }

    int px = 0, py = 0;
    input_pointer_position(&px, &py);
    CompRect at = { px, py, 1, 1 };
    CompOutput *o = output_of(&at);
    if (!o)
        o = comp.output_count > 0 ? &comp.outputs[0] : NULL;
    if (!o)
        return;

    CompEffect *e = open_mode(bound_instance, o, data + 2, len - 2, selected);
    if (!e)
        return;

    ((CsData *)e->data)->external = true;
    active = e;
    effects_add(e);
    mark_dirty(e->data);
}

/* Whether this effect is available to be driven that way at all: the
 * module is compiled in, enabled in the config, and has been given its
 * instance. What _KICOMP_EFFECTS answers with. */
bool cover_switch_available(void)
{
    return bound_instance != NULL;
}

static void cs_init(const CompEffectInstance *self)
{
    const CsConfig *cfg = self->config;

    /* Remembered whether or not there is a hotkey: a window manager can
     * drive this mode with no key of ours bound at all, which is the
     * arrangement this effect is really for. */
    bound_instance = self;

    if (!cfg->hotkey[0])
        return;

    char buf[sizeof(cfg->hotkey)];
    snprintf(buf, sizeof(buf), "%s", cfg->hotkey);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*tok)
            input_bind_hotkey(tok, cs_toggle, (void *)self);
    }
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static void cs_defaults(void *config)
{
    CsConfig *c = config;
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", "Meta+C");
    c->angle = 60.0f;
    c->perspective = 1.2f;
    c->cover = 0.45f;
    c->gap = 0.17f;
    c->step = 0.055f;
    c->depth = 260.0f;
    c->visible = 4;
    c->background = 0.82f;
    c->other_desktops = true;
    c->live_windows = COMP_LIVE_INHERIT;
    c->follow_desktop = true;
    c->labels = true;
    c->label_y = 0.86f;
    c->label_width = 640;
    c->wrap = true;
}

static bool cs_config_key(void *config, const char *key, const char *value)
{
    CsConfig *c = config;

    if (!strcmp(key, "hotkey")) {
        snprintf(c->hotkey, sizeof(c->hotkey), "%s", value);
        return true;
    }
    if (!strcmp(key, "angle"))       { c->angle = (float)atof(value); return true; }
    if (!strcmp(key, "perspective")) { c->perspective = (float)atof(value); return true; }
    if (!strcmp(key, "cover"))       { c->cover = (float)atof(value); return true; }
    if (!strcmp(key, "gap"))         { c->gap = (float)atof(value); return true; }
    if (!strcmp(key, "step"))        { c->step = (float)atof(value); return true; }
    if (!strcmp(key, "depth"))       { c->depth = (float)atof(value); return true; }
    if (!strcmp(key, "visible"))     { c->visible = atoi(value); return true; }
    if (!strcmp(key, "background"))  { c->background = (float)atof(value); return true; }
    if (!strcmp(key, "other_desktops")) { c->other_desktops = atoi(value) != 0; return true; }
    if (!strcmp(key, "live_windows")) {
        int live = comp_live_windows_parse(value);
        if (live < 0 && (!strcmp(value, "0") || !strcmp(value, "1")))
            live = atoi(value) ? COMP_LIVE_ALL : COMP_LIVE_DESKTOP;
        if (live < 0) {
            fprintf(stderr, "kicomp: config: unknown live_windows '%s'\n", value);
            return false;
        }
        c->live_windows = (CompLiveWindows)live;
        return true;
    }
    if (!strcmp(key, "follow_desktop")) { c->follow_desktop = atoi(value) != 0; return true; }
    if (!strcmp(key, "labels"))      { c->labels = atoi(value) != 0; return true; }
    if (!strcmp(key, "label_y"))     { c->label_y = (float)atof(value); return true; }
    if (!strcmp(key, "label_width")) { c->label_width = atoi(value); return true; }
    if (!strcmp(key, "wrap"))        { c->wrap = atoi(value) != 0; return true; }
    return false;
}

const CompEffectModule effect_cover_switch = {
    .name             = "cover-switch",
    .default_enabled  = true,
    .default_duration = 0.9,
    .default_easing   = COMP_EASE_OUT,
    .default_events   = 0,        /* its own hotkey, or kiwm drives it */
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY),

    .init             = cs_init,
    .config_size      = sizeof(CsConfig),
    .config_defaults  = cs_defaults,
    .config_key       = cs_config_key,
};
