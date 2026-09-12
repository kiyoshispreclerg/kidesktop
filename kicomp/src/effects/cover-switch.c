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
    float dim;            /* the ones that are not selected */
    float background;     /* opacity of the ground under everything */
    bool  labels;
    bool  wrap;           /* stepping past the end comes back round */
} CsConfig;

typedef struct {
    CompWindow *win;
    CompRect home;
    char title[TITLE_MAX];
    CompTextImage *label;
} CsItem;

typedef struct {
    int output_id;

    CsItem items[MAX_ITEMS];
    int count;

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
} CsData;

static const CompEffectOps cs_ops;
static CompEffect *active;

static void phase_to(CsData *d, float to, double now);

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

static bool eligible(const CompWindow *w, const CompEffectInstance *self, int output_id)
{
    if (!w->mapped || w->input_only || w->zombie || w->wm_layer[0])
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

    input_release();

    if (activate_it && d->selected >= 0 && d->selected < d->count) {
        /* The one place this effect touches the session: the window the
         * user landed on is raised and focused, once, on the way out. */
        CompWindow *w = d->items[d->selected].win;
        if (w && w->mapped && !w->zombie)
            activate(w);
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

static void on_button(void *data, int root_x, int root_y, uint8_t button, bool pressed)
{
    CompEffect *e = data;
    (void)root_x;
    (void)root_y;

    if (!pressed)
        return;
    if (button == 4)              /* wheel up */
        step_selection(e, -1);
    else if (button == 5)         /* wheel down */
        step_selection(e, +1);
    else if (button == 1)
        close_mode(e, true);
    else if (button == 3)
        close_mode(e, false);
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

static void cs_update(CompEffect *e, double now)
{
    CsData *d = e->data;

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

    /* Two passes over the scene: place every cover, then put the nodes
     * in back-to-front order. There is no depth buffer -- the renderers
     * draw the list in order -- so the order *is* the depth, and a row
     * drawn in stacking order would put the far covers over the near
     * ones. */
    for (int n = 0; n < s->count; n++) {
        CompSceneNode *node = &s->nodes[n];
        int i = index_of(d, node->win);

        if (i < 0) {
            /* Not in the row: a dock, or a window that appeared after
             * the mode opened. Faded out with the ground rather than
             * left sitting on top of it. */
            node->opacity *= 1.0f - cfg->background * alive;
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

        float d_slot = fabsf(slot);
        float lit = d_slot < 0.5f ? 1.0f : cfg->dim;
        node->opacity *= lit * alive;
    }

    /* Back to front: the furthest from the middle first.
     *
     * Adjacent swaps only, repeated until a pass changes nothing. Moving
     * a node shifts every index after it, so a sort that picks nodes out
     * of the list by index while it walks that same list scrambles it --
     * swapping neighbours is the one move whose effect on the indices is
     * small enough to reason about. Nodes that are not covers are never
     * compared, so docks and anything that appeared after the mode
     * opened keep the places the scene gave them. */
    for (int pass = 0; pass < s->count; pass++) {
        bool swapped = false;
        for (int a = 0; a + 1 < s->count; a++) {
            int ia = index_of(d, s->nodes[a].win);
            int ib = index_of(d, s->nodes[a + 1].win);
            if (ia < 0 || ib < 0)
                continue;
            if (fabsf(slot_of(d, ib)) > fabsf(slot_of(d, ia))) {
                scene_move_node(s, a + 1, a);
                swapped = true;
            }
        }
        if (!swapped)
            break;
    }

    /* The title of the one in front, under the row. */
    if (cfg->labels && d->selected >= 0 && d->selected < d->count) {
        CompTextImage *img = d->items[d->selected].label;
        if (img) {
            int tw = text_width(img), th = text_height(img);
            CompRect where = {
                o->rect.x + (o->rect.w - tw) / 2,
                o->rect.y + o->rect.h - o->rect.h / 8 - th / 2,
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
    for (int i = 0; i < d->count; i++)
        if (d->items[i].label)
            text_free(d->items[i].label);
    free(d);
    e->data = NULL;
}

static const CompEffectOps cs_ops = {
    .name       = "cover-switch",
    .update     = cs_update,
    .apply      = cs_apply,
    .finished   = cs_finished,
    .destroy    = cs_destroy,
    .damage_map = cs_damage_map,
};

/* ------------------------------------------------------------------ */
/* the trigger                                                         */
/* ------------------------------------------------------------------ */

static void cs_toggle(void *data)
{
    const CompEffectInstance *self = data;
    const CsConfig *cfg = self->config;

    if (active) {
        step_selection(active, +1);   /* the hotkey again walks the row */
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

    CompEffect *e = calloc(1, sizeof(*e));
    CsData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->output_id = o->id;

    /* Top of the stack first: the row reads the way the user's own
     * most-recent order does, which is the order Alt+Tab walks. */
    for (CompWindow *w = comp.stack; w && d->count < MAX_ITEMS; w = w->next) {
        if (!eligible(w, self, d->output_id))
            continue;
        CsItem *it = &d->items[d->count++];
        it->win = w;
        it->home = window_rect(w);
        read_title(w, it->title, sizeof(it->title));
        if (cfg->labels)
            it->label = text_render(it->title, text_theme_style(), 640);
    }

    if (d->count < 2) {
        cs_destroy(&(CompEffect){ .data = d });
        free(e);
        return;
    }

    /* comp.stack runs bottom to top, so the list came out that way:
     * reverse it, and start on the one under the active window. */
    for (int i = 0, j = d->count - 1; i < j; i++, j--) {
        CsItem tmp = d->items[i];
        d->items[i] = d->items[j];
        d->items[j] = tmp;
    }

    e->ops = &cs_ops;
    e->instance = self;
    e->window = NULL;
    e->start_time = comp_now_ms();
    e->duration = 0.0;            /* finished() decides */
    e->data = d;

    d->selected = 1;              /* the one behind the active window */
    d->pos = 0.0f;
    d->pos_from = 0.0f;
    d->pos_time = comp_now_ms();
    d->phase = 0.0f;              /* every window still exactly where it is */
    phase_to(d, 1.0f, comp_now_ms());

    if (!input_grab(&cs_input, e)) {
        cs_destroy(e);
        free(e);
        return;
    }

    active = e;
    effects_add(e);
    mark_dirty(d);
}

static void cs_init(const CompEffectInstance *self)
{
    const CsConfig *cfg = self->config;
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
    c->dim = 0.7f;
    c->background = 0.82f;
    c->labels = true;
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
    if (!strcmp(key, "dim"))         { c->dim = (float)atof(value); return true; }
    if (!strcmp(key, "background"))  { c->background = (float)atof(value); return true; }
    if (!strcmp(key, "labels"))      { c->labels = atoi(value) != 0; return true; }
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
