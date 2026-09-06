/* See unredirect.h -- one output handed to the window that fills it. */
#include "unredirect.h"
#include "window.h"
#include "output.h"
#include "renderer.h"
#include "effect.h"
#include "input.h"
#include "transform.h"

#include <xcb/composite.h>
#include <xcb/xfixes.h>

#include <stdlib.h>

/* Which window, if any, each output is currently handed over to. Parallel
 * to comp.outputs and rebuilt from output ids rather than indices: an
 * output list that changes under a RandR event must not leave a window
 * unredirected against an output that no longer exists. */
#define MAX_HANDOVERS MAX_OUTPUTS

typedef struct {
    int output_id;
    CompWindow *win;
    CompRect physical;    /* the hole this one needs in the overlay */
} Handover;

static Handover held[MAX_HANDOVERS];
static int held_count;

static Handover *held_for(int output_id)
{
    for (int i = 0; i < held_count; i++)
        if (held[i].output_id == output_id)
            return &held[i];
    return NULL;
}

bool unredirect_holds(const CompOutput *o)
{
    return o && held_for(o->id) != NULL;
}

/* ------------------------------------------------------------------ */
/* the overlay's shape                                                  */
/* ------------------------------------------------------------------ */

/* The compositor's own output covers the screen; every output handed
 * over is a rectangle it must stop covering. Rebuilt whole rather than
 * edited, because "the overlay covers everything except these" is the
 * entire state and describing it twice is how the two drift apart. */
static void overlay_reshape(void)
{
    if (comp.overlay == XCB_NONE || !comp.caps.xfixes)
        return;

    if (held_count == 0) {
        /* Unrestricted: the shape a compositor that is not standing aside
         * for anyone has. */
        xcb_xfixes_set_window_shape_region(comp.conn, comp.overlay,
                                           XCB_SHAPE_SK_BOUNDING, 0, 0,
                                           XCB_XFIXES_REGION_NONE);
        return;
    }

    xcb_rectangle_t whole = { 0, 0, (uint16_t)comp.root_w, (uint16_t)comp.root_h };
    xcb_xfixes_region_t region = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, region, 1, &whole);

    xcb_rectangle_t holes[MAX_HANDOVERS];
    for (int i = 0; i < held_count; i++) {
        holes[i].x = (int16_t)held[i].physical.x;
        holes[i].y = (int16_t)held[i].physical.y;
        holes[i].width = (uint16_t)held[i].physical.w;
        holes[i].height = (uint16_t)held[i].physical.h;
    }

    xcb_xfixes_region_t cut = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, cut, (uint32_t)held_count, holes);
    xcb_xfixes_subtract_region(comp.conn, region, cut, region);

    xcb_xfixes_set_window_shape_region(comp.conn, comp.overlay,
                                       XCB_SHAPE_SK_BOUNDING, 0, 0, region);

    xcb_xfixes_destroy_region(comp.conn, cut);
    xcb_xfixes_destroy_region(comp.conn, region);
}

/* ------------------------------------------------------------------ */
/* taking and giving back                                               */
/* ------------------------------------------------------------------ */

/* How many outputs this window is currently holding. A window bigger
 * than one monitor -- one covering the whole screen, with two monitors
 * under it -- is handed each output separately (each needs its own hole
 * in the overlay), but it is one window and Composite redirects it once:
 * unredirecting it twice is an error, and redirecting it back after the
 * first output goes would leave the second one showing nothing. */
static int outputs_held_by(const CompWindow *w)
{
    int n = 0;
    for (int i = 0; i < held_count; i++)
        if (held[i].win == w)
            n++;
    return n;
}

static void take(CompOutput *o, CompWindow *w)
{
    if (held_count >= MAX_HANDOVERS)
        return;

    /* The window draws to the screen from here on, so the pixmap kicomp
     * was reading is about to stop being what is on screen -- and X frees
     * it when the redirection ends. Once per window, however many outputs
     * it ends up covering. */
    if (outputs_held_by(w) == 0) {
        xcb_composite_unredirect_window(comp.conn, w->id,
                                        XCB_COMPOSITE_REDIRECT_MANUAL);
        renderer_window_invalidate(w);
    }

    held[held_count].output_id = o->id;
    held[held_count].win = w;
    held[held_count].physical = o->physical;
    held_count++;

    overlay_reshape();
    xcb_flush(comp.conn);

    comp_info("%s handed to window 0x%x (unredirected)", o->name, w->id);
}

/* Is this still a window the compositor knows about? A handover holds a
 * pointer to one, and the window it was handed to is precisely the kind
 * that goes away without warning -- a game exits, and the entry left
 * behind must not be dereferenced to find that out. */
static bool still_tracked(const CompWindow *w)
{
    for (CompWindow *it = comp.stack; it; it = it->next)
        if (it == w)
            return true;
    return false;
}

static void give_back_index(int index)
{
    CompWindow *w = held[index].win;
    int output_id = held[index].output_id;

    if (!still_tracked(w))
        w = NULL;   /* gone: nothing to redirect, and nothing to read */

    held[index] = held[held_count - 1];
    held_count--;

    /* Redirected again once the last output it was holding is back, and
     * its contents named afresh on the next paint: the pixmap it had
     * before the handover is gone. */
    if (w && !w->zombie && outputs_held_by(w) == 0) {
        xcb_composite_redirect_window(comp.conn, w->id,
                                      XCB_COMPOSITE_REDIRECT_MANUAL);
        renderer_window_invalidate(w);
    }

    overlay_reshape();

    /* Whatever was on that output while kicomp was not painting it is not
     * what kicomp last drew there. */
    for (int i = 0; i < comp.output_count; i++) {
        if (comp.outputs[i].id != output_id)
            continue;
        output_damage_rect(&comp.outputs[i].rect);
        comp_info("%s taken back (redirected)", comp.outputs[i].name);
        break;
    }

    xcb_flush(comp.conn);
}

void unredirect_release_all(void)
{
    while (held_count > 0)
        give_back_index(held_count - 1);
}

/* ------------------------------------------------------------------ */
/* who qualifies                                                        */
/* ------------------------------------------------------------------ */

/* Does this window cover the whole of this output with pixels the
 * compositor is sure are opaque? window.h's opaque rectangle is the
 * *client's*, never the frame's, which is exactly the honesty needed
 * here: a frame with rounded corners does not cover its output, and a
 * game inside one is not eligible until the WM has made it borderless. */
static bool fills_output(CompWindow *w, const CompOutput *o)
{
    if (w->shaped || w->opacity < 1.0)
        return false;

    CompRect opaque = window_opaque_rect(w);
    if (opaque.w <= 0 || opaque.h <= 0)
        return false;

    return opaque.x <= o->rect.x && opaque.y <= o->rect.y &&
           opaque.x + opaque.w >= o->rect.x + o->rect.w &&
           opaque.y + opaque.h >= o->rect.y + o->rect.h;
}

/* The window this output should be handed to, or NULL for "keep
 * compositing it".
 *
 * Walks the stack from the top: the first mapped window that touches this
 * output is the only candidate there can be, because anything above it
 * would have to be drawn over it and kicomp will not be drawing anything
 * there. Which also means the test for "nothing else is visible on this
 * output" is simply that the first one found is the one that fills it. */
static CompWindow *candidate_for(CompOutput *o)
{
    if (o->scale != 1.0f)
        return NULL;    /* the compositor is magnifying this output */
    if (!comp_transform_is_identity(&o->view))
        return NULL;    /* ...or a lens is (zoom) */

    CompWindow *top = NULL;
    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (!w->mapped || w->input_only || w->zombie || w->stowed || w->held)
            continue;
        if (w->opacity <= 0.0)
            continue;

        CompRect r = window_rect(w);
        CompRect hit;
        if (!rect_intersect(&r, &o->rect, &hit))
            continue;

        top = w;   /* the stack is bottom-first, so the last one wins */
    }

    if (!top || !fills_output(top, o))
        return NULL;

    /* kiwm's own overlays (the alt-tab OSD, the outline rectangles) are
     * windows like any other, and one of them covering an output would
     * qualify on the letter of the rule while being the clearest possible
     * sign that something is happening on this screen. */
    if (top->wm_layer[0])
        return NULL;

    /* Neither is the desktop worth handing over. A wallpaper does cover
     * its output, and on a desktop with nothing open it would qualify on
     * every count -- but a compositor that is not being asked to draw
     * anything is already costing nothing, and standing aside for a
     * static image buys exactly that in exchange for the next window,
     * notification or effect arriving to a screen that has to be taken
     * back first. This is for the window that is *drawing*: a game, a
     * video, a full-screen application. */
    if (top->type == COMP_WINDOW_DESKTOP || top->type == COMP_WINDOW_DOCK)
        return NULL;

    return top;
}

/* ------------------------------------------------------------------ */

void unredirect_update(void)
{
    /* Everything the compositor does *itself* on top of the windows --
     * an animation, a mode holding the keyboard -- needs the screen back,
     * everywhere. Cheaper to ask once than to work out which outputs an
     * effect is going to touch, and an effect that touches none of them
     * for a moment is not worth a handover that lasts that moment. */
    if (!comp.unredirect || effects_active() || input_grabbed()) {
        unredirect_release_all();
        return;
    }

    /* Windows that stopped existing while holding an output: dropped
     * first, so nothing below reads one. */
    for (int i = held_count - 1; i >= 0; i--)
        if (!still_tracked(held[i].win))
            give_back_index(i);

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        CompWindow *want = candidate_for(o);
        Handover *have = held_for(o->id);

        if (have && have->win == want)
            continue;                       /* nothing changed */

        if (have) {
            for (int j = 0; j < held_count; j++) {
                if (&held[j] == have) {
                    give_back_index(j);
                    break;
                }
            }
        }
        if (want)
            take(o, want);
    }

    /* An output that went away with the handover still on it: RandR can
     * remove a CRTC while a window is filling it, and a hole in the
     * overlay for a rectangle that is no longer on any screen is a hole
     * nothing ever fills. */
    for (int i = held_count - 1; i >= 0; i--) {
        bool alive = false;
        for (int j = 0; j < comp.output_count && !alive; j++)
            alive = comp.outputs[j].id == held[i].output_id;
        if (!alive)
            give_back_index(i);
    }
}
