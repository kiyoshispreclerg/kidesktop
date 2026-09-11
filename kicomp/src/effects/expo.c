/*
 * expo: every virtual desktop at once, laid out the way the pager lays
 * them out, to walk into one.
 *
 * The second effect that is a *mode* (see input.h, and show-windows for
 * the first). Where that one arranges the windows of the desktop you are
 * on, this one leaves the windows where they are and shrinks whole
 * desktops: each cell of the grid is that desktop's screen, drawn at a
 * fraction of its size, with its windows in their own places and their
 * own stacking order. Clicking a cell goes there, and the window you
 * clicked on comes with you -- focused, in front, from the first frame
 * of the way out.
 *
 * The desktops that are not on screen are drawn from the last picture the
 * compositor has of their windows (comp.h's keep_stowed): X frees an
 * unmapped window's contents, so what is shown is the moment each window
 * was put away. That is what every expo in every desktop shows, and the
 * alternative -- empty cells -- is not an expo.
 *
 * The layout is not this effect's to invent. Columns and rows come from
 * _NET_DESKTOP_LAYOUT and the desktop count, which is exactly what the
 * pager in the panel is drawing from, so the two always agree about
 * where desktop 3 is (desktop.h).
 *
 * kicomp.conf:
 *
 *   [effect:expo]
 *   enabled  = 1
 *   hotkey   = Meta+E           # the same key comes back out
 *   duration = 1.5
 *   easing   = out
 *   margin   = 24               # around the grid of desktops
 *   padding  = 24               # between the cells -- the same gap by
 *                               # default, so it reads as one spacing
 *   dim      = 0.82             # the desktops other than the selected one
 *   background = #000000        # the ground they are laid out on
 *   live_windows = desktop      # desktop | active_only | active | all --
 *                               # which of the other desktops' windows are
 *                               # kept drawing while the grid is up
 *   arrange  = stack            # stack | grid -- windows as they are, or
 *                               # tidied into a little grid of their own
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../input.h"
#include "../window.h"
#include "../desktop.h"
#include "../transform.h"
#include "../scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_DESKTOPS 32
#define MAX_ITEMS 128

typedef enum {
    ARRANGE_STACK,   /* windows where they really are on their desktop */
    ARRANGE_GRID,    /* tidied into a grid inside their cell */
} Arrange;

/* Which windows keep *moving* while the grid is up.
 *
 * A window on a desktop nobody is showing is not on screen, and X keeps
 * nothing of a window that is not on screen: what a cell draws is the
 * picture it had when that desktop was left. The WM can put one back on
 * screen for a moment so there is a live picture instead
 * (kiwm/PROTOCOL.md's _KIWM_HOLD_WINDOW) -- and the cost of that is the
 * application drawing again, which is exactly the cost of not having
 * left it drawing in the first place. So it is asked for by name.
 *
 * The desktop you are on is not part of the question: its windows are on
 * screen and therefore live whatever is set here. */
typedef enum {
    LIVE_DESKTOP,      /* only the desktop you are on -- nothing is held */
    LIVE_ACTIVE_ONLY,  /* ...and the last-used window of each other desktop */
    LIVE_ACTIVE,       /* the same set: a separate name because what it says
                        * is "this desktop *and* the others' active ones",
                        * which is what someone writing it means, even
                        * though the first half needs no asking */
    LIVE_ALL,          /* every window of every desktop */
} LiveWindows;

typedef struct {
    CompWindow *win;
    int desktop;

    CompRect home;      /* where it really is (or was, when stowed) */
    CompRect from, to, current;
    float alpha_from, alpha_to, alpha;
    bool stowed;        /* only its last picture exists */

    /* Scenery rather than a window to pick: the wallpaper below
     * everything, the panels above it. A cell is meant to look like the
     * desktop, which means the whole desktop -- with its panels, and with
     * its own wallpaper where there is one per desktop (xisback keeps a
     * layer per output and desktop, so those arrive as ordinary windows
     * of their own and land in their own cells by themselves). */
    bool scenery;
    bool below;         /* scenery drawn under the windows, not over */
    bool sticky;        /* on every desktop: drawn once per cell */
} ExItem;

typedef struct {
    ExItem items[MAX_ITEMS];
    int count;

    int output_id;
    int desktops;
    int cols, rows;
    int current_desktop;   /* the one the output was showing when we opened */
    int selected;          /* the chosen cell (arrow keys), -1 for none */

    /* Where each desktop's cell is, in root coordinates. */
    CompRect cell[MAX_DESKTOPS];
    float scale;           /* output -> cell */

    bool closing;
    /* The grid is up and a leg is running that only moves windows
     * within it (settle, after a drop): the leg's progress is theirs,
     * not the grid's -- which is at full spread and stays there. */
    bool settling;
    int enter_desktop;     /* desktop to switch to on the way out, -1 = none */
    CompWindow *enter_window;

    double leg_start;
    double leg_ms;

    /* When the windows of the other desktops were last asked to stay on
     * screen. The ask expires by itself, so this is what makes it a
     * renewal rather than a promise (LiveWindows). */
    double held_at;

    /* A window picked up with the button and carried across the grid
     * (on_button, on_motion): its picture follows the pointer, and where
     * it is let go decides its desktop. NULL when nothing is held. Until
     * the pointer has moved DRAG_SLOP away from where it went down this
     * is still a click that may yet be released in place. */
    ExItem *drag;
    bool drag_moving;
    int drag_x, drag_y;      /* where the button went down */
    int drag_dx, drag_dy;    /* pointer's offset inside the picture */
} ExData;

#define DRAG_SLOP 8

typedef struct {
    char hotkey[64];
    int margin;
    int padding;
    float dim;
    Arrange arrange;
    float bg[3];       /* the ground the desktops are laid out on */
    LiveWindows live;
} ExConfig;

static const CompEffectOps ex_ops;
static CompEffect *active;

static ExItem *item_for(ExData *d, const CompWindow *win);

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

static CompOutput *output_by_id(int id)
{
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == id)
            return &comp.outputs[i];
    return NULL;
}

static CompRect lerp_rect(const CompRect *a, const CompRect *b, float p)
{
    CompRect r;
    r.x = (int)(comp_lerp((float)a->x, (float)b->x, p) + 0.5f);
    r.y = (int)(comp_lerp((float)a->y, (float)b->y, p) + 0.5f);
    r.w = (int)(comp_lerp((float)a->w, (float)b->w, p) + 0.5f);
    r.h = (int)(comp_lerp((float)a->h, (float)b->h, p) + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

/* Where scenery may be drawn, at this point in the animation: the whole
 * screen while it is still at home, its own cell once it has arrived, and
 * the rectangle in between while it travels.
 *
 * Fixed at the cell it would cut the wallpaper off from the first frame,
 * long before it has shrunk to fit; fixed at the screen it would let a
 * panel wider than this monitor spill into the neighbouring cell once it
 * has. Interpolating is the only one of the three that is right at both
 * ends -- and at the start there is nothing to clip anyway, since a
 * window's part on another output is drawn by that output's own scene. */
static CompRect clip_bound(const CompRect *screen, const CompRect *cell,
                           float spread)
{
    CompRect r;
    r.x = (int)(comp_lerp((float)screen->x, (float)cell->x, spread) + 0.5f);
    r.y = (int)(comp_lerp((float)screen->y, (float)cell->y, spread) + 0.5f);
    r.w = (int)(comp_lerp((float)screen->w, (float)cell->w, spread) + 0.5f);
    r.h = (int)(comp_lerp((float)screen->h, (float)cell->h, spread) + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

/* One window's place inside its desktop's cell: the same place it has on
 * its own desktop, scaled down. That is the whole idea of an expo -- what
 * you are looking at is the desktop, not a list of its windows -- so a
 * window half off the left edge is drawn half off the cell's left edge
 * too, and the cell is clipped to itself by the layout around it. */
static CompRect place_in_cell(const ExData *d, const CompOutput *o,
                              const CompRect *home, int desktop)
{
    const CompRect *cell = &d->cell[desktop];

    CompRect r;
    r.x = cell->x + (int)((float)(home->x - o->rect.x) * d->scale + 0.5f);
    r.y = cell->y + (int)((float)(home->y - o->rect.y) * d->scale + 0.5f);
    r.w = (int)((float)home->w * d->scale + 0.5f);
    r.h = (int)((float)home->h * d->scale + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

/* The tidied alternative: the desktop's windows share their cell as a
 * little grid of their own, each one shrunk to fit and never enlarged. */
static void arrange_grid(ExData *d, int desktop)
{
    int n = 0;
    for (int i = 0; i < d->count; i++)
        if (d->items[i].desktop == desktop && !d->items[i].scenery &&
            !d->items[i].sticky)
            n++;
    if (n == 0)
        return;

    int cols = (int)ceil(sqrt((double)n));
    if (cols < 1)
        cols = 1;
    int rows = (n + cols - 1) / cols;

    const CompRect *cell = &d->cell[desktop];
    int pad = 4;
    int cw = (cell->w - pad * (cols + 1)) / cols;
    int ch = (cell->h - pad * (rows + 1)) / rows;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    int k = 0;
    for (int i = 0; i < d->count; i++) {
        ExItem *it = &d->items[i];
        if (it->desktop != desktop || it->scenery || it->sticky)
            continue;

        float sx = (float)cw / (float)(it->home.w > 0 ? it->home.w : 1);
        float sy = (float)ch / (float)(it->home.h > 0 ? it->home.h : 1);
        float s = sx < sy ? sx : sy;
        if (s > 1.0f)
            s = 1.0f;

        int col = k % cols, row = k / cols;
        int w = (int)((float)it->home.w * s + 0.5f);
        int h = (int)((float)it->home.h * s + 0.5f);
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        it->to.x = cell->x + pad + col * (cw + pad) + (cw - w) / 2;
        it->to.y = cell->y + pad + row * (ch + pad) + (ch - h) / 2;
        it->to.w = w;
        it->to.h = h;
        k++;
    }
}

static void layout(CompEffect *e, double now)
{
    ExData *d = e->data;
    const ExConfig *cfg = e->instance->config;

    CompOutput *o = output_by_id(d->output_id);
    if (!o)
        return;

    int m = cfg->margin, p = cfg->padding;
    int area_w = o->rect.w - m * 2;
    int area_h = o->rect.h - m * 2;
    if (area_w < 1) area_w = 1;
    if (area_h < 1) area_h = 1;

    int cw = (area_w - p * (d->cols - 1)) / d->cols;
    int ch = (area_h - p * (d->rows - 1)) / d->rows;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    /* A cell is the screen, so it keeps the screen's proportions: a
     * desktop drawn wider than it is makes every window in it the wrong
     * shape, and an expo is supposed to look like the desktop. */
    float sx = (float)cw / (float)(o->rect.w > 0 ? o->rect.w : 1);
    float sy = (float)ch / (float)(o->rect.h > 0 ? o->rect.h : 1);
    d->scale = sx < sy ? sx : sy;

    int shown_w = (int)((float)o->rect.w * d->scale + 0.5f);
    int shown_h = (int)((float)o->rect.h * d->scale + 0.5f);

    /* Centre the whole block of cells in what is left over. */
    int block_w = shown_w * d->cols + p * (d->cols - 1);
    int block_h = shown_h * d->rows + p * (d->rows - 1);
    int ox = o->rect.x + (o->rect.w - block_w) / 2;
    int oy = o->rect.y + (o->rect.h - block_h) / 2;

    for (int i = 0; i < d->desktops && i < MAX_DESKTOPS; i++) {
        int col = i % d->cols, row = i / d->cols;
        d->cell[i].x = ox + col * (shown_w + p);
        d->cell[i].y = oy + row * (shown_h + p);
        d->cell[i].w = shown_w;
        d->cell[i].h = shown_h;
    }

    for (int i = 0; i < d->count; i++) {
        ExItem *it = &d->items[i];
        it->from = it->current;
        it->alpha_from = it->alpha;
        it->alpha_to = 1.0f;
        it->to = place_in_cell(d, o, &it->home, it->desktop);
    }

    if (cfg->arrange == ARRANGE_GRID)
        for (int i = 0; i < d->desktops && i < MAX_DESKTOPS; i++)
            arrange_grid(d, i);

    /* Only the desktop you were on travels: it shrinks from the screen
     * into its cell, and that one motion is the whole of the way in.
     * The other desktops were not on screen, so there is nowhere for
     * them to shrink *from* -- flying them in from a full-size position
     * nobody ever saw was four desktops converging on the middle, with
     * the one you were using lost among them. They are simply placed,
     * already in their cells, and fade in as the grid does. */
    for (int i = 0; i < d->count; i++) {
        ExItem *it = &d->items[i];
        if (it->sticky || it->desktop == d->current_desktop)
            continue;
        it->from = it->current = it->to;
        it->alpha = it->alpha_from = 0.0f;
    }

    d->leg_start = now;
    d->leg_ms = effect_instance_duration(e->instance);
    output_damage_rect(&o->rect);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static int desktop_at(const ExData *d, int x, int y)
{
    for (int i = 0; i < d->desktops && i < MAX_DESKTOPS; i++)
        if (x >= d->cell[i].x && x < d->cell[i].x + d->cell[i].w &&
            y >= d->cell[i].y && y < d->cell[i].y + d->cell[i].h)
            return i;
    return -1;
}

/* The topmost window drawn under a point, which is the last one in the
 * list that covers it -- comp.stack is bottom-first and the items were
 * collected in that order, so the same walk that draws them answers
 * "which one is on top here". */
static CompWindow *window_at(const ExData *d, int x, int y)
{
    CompWindow *hit = NULL;
    for (int i = 0; i < d->count; i++) {
        const ExItem *it = &d->items[i];
        if (it->scenery)
            continue;               /* the wallpaper is not a choice */
        if (x >= it->current.x && x < it->current.x + it->current.w &&
            y >= it->current.y && y < it->current.y + it->current.h)
            hit = it->win;
    }
    return hit;
}

static void close_mode(CompEffect *e, int enter_desktop, CompWindow *pick)
{
    ExData *d = e->data;
    if (d->closing)
        return;

    d->closing = true;
    d->settling = false;
    d->enter_desktop = enter_desktop;
    d->enter_window = pick;
    d->drag = NULL;          /* whatever was held goes home with its cell */
    input_release();

    /* Asked now rather than when the animation ends: the WM has a desktop
     * to switch and windows to map, and the sooner it starts the closer
     * the two halves land. What it does arrives back as ordinary events
     * -- windows mapping, the property changing -- and the animation
     * carries on regardless. */
    if (enter_desktop >= 0 && enter_desktop != d->current_desktop)
        desktop_request_switch(output_by_id(d->output_id), enter_desktop);

    /* The mirror of layout(): only the desktop being walked into grows
     * back to the screen. */
    int going = enter_desktop >= 0 ? enter_desktop : d->current_desktop;

    for (int i = 0; i < d->count; i++) {
        ExItem *it = &d->items[i];
        it->from = it->current;
        it->alpha_from = it->alpha;
        if (it->sticky || it->desktop == going) {
            it->to = it->home;
            it->alpha_to = 1.0f;
        } else {
            /* The desktops nobody chose stay in their cells and fade
             * out: their windows are about to be unmapped again (or
             * never were mapped), and a window sliding home to a desktop
             * you are not on has no home to arrive at. */
            it->to = it->current;
            it->alpha_to = 0.0f;
        }
    }

    d->leg_start = comp_now_ms();
    d->leg_ms = effect_instance_duration(e->instance);

    CompOutput *o = output_by_id(d->output_id);
    if (o)
        output_damage_rect(&o->rect);
}

static bool on_key(void *data, xcb_keysym_t sym, const char *text, uint16_t mods)
{
    CompEffect *e = data;
    ExData *d = e->data;
    (void)text; (void)mods;

    switch (sym) {
    case 0xff1b:                            /* Escape */
        close_mode(e, -1, NULL);
        return true;
    case 0xff0d:                            /* Return */
    case 0xff8d:
        close_mode(e, d->selected, NULL);
        return true;
    case 0xff51: if (d->selected > 0) d->selected--; break;            /* Left */
    case 0xff53: if (d->selected + 1 < d->desktops) d->selected++; break;
    case 0xff52: if (d->selected - d->cols >= 0) d->selected -= d->cols; break;
    case 0xff54: if (d->selected + d->cols < d->desktops) d->selected += d->cols; break;
    default:
        return false;
    }

    CompOutput *o = output_by_id(d->output_id);
    if (o)
        output_damage_rect(&o->rect);
    return true;
}

/* Every window back to its place in its cell, from wherever it is now,
 * over one leg: what follows a drop, when one window has changed cell
 * and (with arrange=grid) the two cells it left and joined have to make
 * room and close up. */
static void settle(CompEffect *e, double now)
{
    ExData *d = e->data;
    const ExConfig *cfg = e->instance->config;

    CompOutput *o = output_by_id(d->output_id);
    if (!o)
        return;

    for (int i = 0; i < d->count; i++) {
        ExItem *it = &d->items[i];
        it->from = it->current;
        it->alpha_from = it->alpha;
        it->alpha_to = 1.0f;
        it->to = place_in_cell(d, o, &it->home, it->desktop);
    }

    if (cfg->arrange == ARRANGE_GRID)
        for (int i = 0; i < d->desktops && i < MAX_DESKTOPS; i++)
            arrange_grid(d, i);

    d->settling = true;
    d->leg_start = now;
    d->leg_ms = effect_instance_duration(e->instance);
    output_damage_rect(&o->rect);
}

static void on_motion(void *data, int root_x, int root_y)
{
    CompEffect *e = data;
    ExData *d = e->data;
    CompOutput *o = output_by_id(d->output_id);

    if (d->drag) {
        /* A press becomes a drag once it has clearly moved: the picture
         * is pinned to the pointer from then on, keeping the offset it
         * was picked up with, and stays pinned however the legs behind
         * it are interpolating (from == to == here). */
        if (!d->drag_moving) {
            int dx = root_x - d->drag_x, dy = root_y - d->drag_y;
            if (dx * dx + dy * dy >= DRAG_SLOP * DRAG_SLOP)
                d->drag_moving = true;
        }
        if (d->drag_moving) {
            ExItem *it = d->drag;
            CompRect r = it->current;
            r.x = root_x - d->drag_dx;
            r.y = root_y - d->drag_dy;
            it->from = it->to = it->current = r;
            if (o)
                output_damage_rect(&o->rect);
        }
    }

    /* Passing over a cell chooses nothing: the pointer crosses every
     * cell on the way to the one meant, and a grid that follows it
     * around answers a question that hasn't been asked. The selection
     * moves by the arrow keys and lands by a click; leaving without
     * either (Escape, a click off the grid) goes back to the desktop
     * that was showing. */
}

static void on_button(void *data, int root_x, int root_y, uint8_t button,
                      bool pressed)
{
    CompEffect *e = data;
    ExData *d = e->data;

    /* Pressing chooses nothing; letting go is what walks into a
     * desktop. So a press on the wrong cell can be slid off and released
     * elsewhere, as a button anywhere else would allow.
     *
     * A press *on a window* picks it up (on_motion): moved to
     * another cell and let go there, the window changes desktop and
     * nothing else -- same place, same size -- and the grid stays up.
     * Released where it was pressed, it is the click it always was. */
    if (pressed) {
        on_motion(data, root_x, root_y);
        if (button == 1 && !d->closing) {
            CompWindow *w = window_at(d, root_x, root_y);
            ExItem *it = w ? item_for(d, w) : NULL;
            /* A window on every desktop has no other desktop to go to. */
            if (it && !it->sticky) {
                d->drag = it;
                d->drag_moving = false;
                d->drag_x = root_x;
                d->drag_y = root_y;
                d->drag_dx = root_x - it->current.x;
                d->drag_dy = root_y - it->current.y;
            }
        }
        return;
    }

    if (button == 1 && d->drag) {
        ExItem *it = d->drag;
        bool moved = d->drag_moving;
        d->drag = NULL;

        if (moved) {
            /* Dropped: on another cell it changes desktop -- asked of the
             * WM, and the cell it now sits in is the answer assumed,
             * since what comes back is a window unmapping or mapping and
             * the grid draws it from its picture either way. Anywhere
             * else it goes back where it came from. */
            int desk = desktop_at(d, root_x, root_y);
            if (desk >= 0 && desk != it->desktop &&
                desktop_request_move(it->win, desk))
                it->desktop = desk;
            settle(e, comp_now_ms());
            return;
        }
    }

    if (button != 1) {
        close_mode(e, -1, NULL);
        return;
    }

    int desk = desktop_at(d, root_x, root_y);
    if (desk < 0) {
        close_mode(e, -1, NULL);   /* the space around the grid: never mind */
        return;
    }

    /* The window under the click comes with you. Which is the point of
     * clicking a window rather than a cell: you are not choosing a
     * desktop and then hunting for the window again. */
    close_mode(e, desk, window_at(d, root_x, root_y));
}

static const CompInputHandler ex_input = {
    .key = on_key,
    .motion = on_motion,
    .button = on_button,
};

/* ------------------------------------------------------------------ */
/* the effect                                                          */
/* ------------------------------------------------------------------ */

static float leg_progress(const ExData *d, const CompEffect *e, double now)
{
    if (d->leg_ms <= 0.0)
        return 1.0f;
    return effect_ease(e, comp_progress(now, d->leg_start, d->leg_ms));
}

static ExItem *item_for(ExData *d, const CompWindow *win)
{
    for (int i = 0; i < d->count; i++)
        if (d->items[i].win == win)
            return &d->items[i];
    return NULL;
}

/* How long each ask is good for, and how often it is renewed. The gap
 * between them is the slack: if this compositor dies with the grid open,
 * every window it was holding goes back where it belongs within the
 * first number, without anything of ours running. */
#define HOLD_MS       1500
#define HOLD_RENEW_MS 500

/* The window used most recently on a desktop -- kicomp's own focus
 * record (comp.h's focus_serial), since X keeps no such history. */
static const ExItem *last_used_on(const ExData *d, int desktop)
{
    const ExItem *best = NULL;

    for (int i = 0; i < d->count; i++) {
        const ExItem *it = &d->items[i];
        if (it->desktop != desktop || it->scenery || it->sticky)
            continue;
        if (!best || it->win->focus_serial > best->win->focus_serial)
            best = it;
    }
    return best;
}

/* Asks the WM to keep the other desktops' windows on screen a little
 * longer, so their cells show what those windows are doing rather than
 * what they were doing when the desktop was left.
 *
 * Only while the grid is *up*: not while it is opening or closing, when
 * every cell is moving and nobody is reading them anyway, and when the
 * windows are about to be put back regardless. */
static void hold_live_windows(CompEffect *e, double now)
{
    ExData *d = e->data;
    const ExConfig *cfg = e->instance->config;

    if (cfg->live == LIVE_DESKTOP)
        return;
    if (d->held_at != 0.0 && now - d->held_at < HOLD_RENEW_MS)
        return;
    d->held_at = now;

    for (int i = 0; i < d->count; i++) {
        const ExItem *it = &d->items[i];

        if (it->scenery || it->sticky)
            continue;
        if (it->desktop == d->current_desktop)
            continue;                   /* on screen already: live for free */
        if (cfg->live != LIVE_ALL && it != last_used_on(d, it->desktop))
            continue;

        desktop_request_hold(it->win, HOLD_MS);
    }
}

/* Where a window's own redrawing lands while the grid is up (effect.h):
 * in its cell, at the size the cell draws it. Without this a video keeps
 * playing in a thumbnail that never changes -- the pixels it damages are
 * the ones where the window really is, which during an expo is nowhere
 * anybody is looking. */
static bool ex_damage_map(const CompEffect *e, const CompWindow *w,
                          const CompRect *in, CompRect *out)
{
    const ExData *d = e->data;
    const ExItem *it = NULL;

    for (int i = 0; i < d->count; i++)
        if (d->items[i].win == w)
            it = &d->items[i];
    if (!it || it->home.w <= 0 || it->home.h <= 0)
        return false;

    float sx = (float)it->current.w / (float)it->home.w;
    float sy = (float)it->current.h / (float)it->home.h;

    /* One pixel of slack all round: the cell is a fraction of the size
     * the damage was measured at, and a rectangle that lands half a
     * pixel short leaves a seam that never repaints. */
    out->x = it->current.x + (int)((float)(in->x - it->home.x) * sx) - 1;
    out->y = it->current.y + (int)((float)(in->y - it->home.y) * sy) - 1;
    out->w = (int)((float)in->w * sx + 0.5f) + 2;
    out->h = (int)((float)in->h * sy + 0.5f) + 2;
    return out->w > 0 && out->h > 0;
}

static void ex_update(CompEffect *e, double now)
{
    ExData *d = e->data;

    float p = leg_progress(d, e, now);

    for (int i = 0; i < d->count; i++) {
        d->items[i].current = lerp_rect(&d->items[i].from, &d->items[i].to, p);
        d->items[i].alpha = comp_lerp(d->items[i].alpha_from,
                                      d->items[i].alpha_to, p);
    }

    if (p < 1.0f) {
        CompOutput *o = output_by_id(d->output_id);
        if (o)
            output_damage_rect(&o->rect);
        for (int i = 0; i < d->count; i++)
            output_damage_rect(&d->items[i].home);
    } else if (!d->closing) {
        /* Standing still, which is the only part of this where anyone is
         * actually looking at the other desktops. */
        hold_live_windows(e, now);
    }
}

/* A node of a window that is not an item but is on this output's screen
 * right now, and so belongs to the desktop the output is showing (see
 * ex_apply's rebuild). Not the WM's own overlays, which are never part of
 * any desktop, and not a window whose middle is on another monitor --
 * the node is only here because it reaches across. */
static bool late_arrival(const CompOutput *o, const CompSceneNode *n)
{
    const CompWindow *w = n->win;
    if (!w->mapped || w->held || w->wm_layer[0])
        return false;
    CompRect centre = { n->geometry.x + n->geometry.w / 2,
                        n->geometry.y + n->geometry.h / 2, 1, 1 };
    CompRect hit;
    return rect_intersect(&centre, &o->rect, &hit);
}

/* Draws `node` where its window would sit in cell `c` at this point of
 * the spread: at home when the grid is down, scaled into the cell when
 * it is up, and in between on the way. */
static void shrink_into_cell(const ExData *d, const CompOutput *o,
                             CompSceneNode *node, int c, float spread)
{
    CompRect target = place_in_cell(d, o, &node->geometry, c);
    CompRect cur = lerp_rect(&node->geometry, &target, spread);

    float sx = (float)cur.w / (float)(node->geometry.w > 0 ? node->geometry.w : 1);
    float sy = (float)cur.h / (float)(node->geometry.h > 0 ? node->geometry.h : 1);

    comp_transform_identity(&node->transform);
    comp_transform_translate(&node->transform, (float)-node->geometry.x,
                             (float)-node->geometry.y);
    comp_transform_scale(&node->transform, sx, sy);
    comp_transform_translate(&node->transform, (float)cur.x, (float)cur.y);

    CompRect vis;
    if (!rect_intersect(&cur, &o->rect, &vis))
        vis = (CompRect){ 0, 0, 0, 0 };
    node->visible_rect = vis;
}

static void ex_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    ExData *d = e->data;
    const ExConfig *cfg = e->instance->config;

    float p = leg_progress(d, e, comp_now_ms());
    float spread = d->closing ? 1.0f - p : (d->settling ? 1.0f : p);

    /* The one cell that moves (layout, close_mode): the desktop you came
     * from on the way in, the one you are going to on the way out. Every
     * other cell stands where it is at full spread and only its opacity
     * follows the leg. */
    int moving_cell = d->closing
        ? (d->enter_desktop >= 0 ? d->enter_desktop : d->current_desktop)
        : d->current_desktop;

    if (o->id == d->output_id) {
        /* The ground first, under everything: the desktops are laid out
         * *somewhere*, not floating over the one you were just using.
         * It fades in and out with the layout, so the way in and the way
         * out are the same gesture reversed (scene.h). */
        scene_set_backdrop(s, &o->rect, cfg->bg[0], cfg->bg[1], cfg->bg[2],
                           spread);
    }

    if (o->id != d->output_id) {
        /* Another monitor: its own desktops are not what this grid is
         * about, and the strip of a window that reaches across fades
         * rather than being dragged into someone else's layout -- the
         * same answer desktop-wall and show-windows give. */
        for (int i = 0; i < s->count; i++)
            if (item_for(d, s->nodes[i].win))
                s->nodes[i].opacity *= comp_lerp(1.0f, 0.0f, spread);
        return;
    }

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        ExItem *it = item_for(d, n->win);
        if (!it)
            continue;

        const CompRect *cur = &it->current;
        float sx = (float)cur->w / (float)(n->geometry.w > 0 ? n->geometry.w : 1);
        float sy = (float)cur->h / (float)(n->geometry.h > 0 ? n->geometry.h : 1);

        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform, (float)-n->geometry.x,
                                 (float)-n->geometry.y);
        comp_transform_scale(&n->transform, sx, sy);
        comp_transform_translate(&n->transform, (float)cur->x, (float)cur->y);

        n->opacity *= it->alpha;

        /* The desktop under the pointer is the one you are about to walk
         * into; the rest stand back. */
        if (d->selected >= 0 && it->desktop != d->selected && it != d->drag) {
            float dim = cfg->dim;
            if (dim < 0.0f) dim = 0.0f;
            if (dim > 1.0f) dim = 1.0f;
            n->opacity *= comp_lerp(1.0f, dim, spread);
        }

        /* A cell *is* this output, scaled -- so clipping scenery to the
         * cell is exactly "the part of it that is on this screen", which
         * is what a panel spanning two monitors, or a wallpaper window
         * larger than one, has to be reduced to. Windows are not clipped
         * that way: one hanging over the edge is still a whole window
         * and is shown whole. */
        CompRect bound = o->rect;
        if (it->scenery && it->desktop >= 0 && it->desktop < d->desktops)
            bound = clip_bound(&o->rect, &d->cell[it->desktop],
                               it->desktop == moving_cell ? spread : 1.0f);

        CompRect vis;
        if (rect_intersect(cur, &bound, &vis))
            n->visible_rect = vis;
        else
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
    }

    /* And now the scenery that belongs to *every* desktop -- the panels,
     * and a wallpaper that isn't published per desktop -- is drawn once
     * into each cell, because a cell is supposed to look like the
     * desktop and a desktop has its panels.
     *
     * One window, several nodes: the scene is a list of things to draw,
     * not a list of windows, so a copy with a different transform draws
     * the same pixmap somewhere else. The order is rebuilt rather than
     * appended to, because within a cell the wallpaper has to be under
     * that cell's windows and the panels over them -- appending would
     * put every panel on top of every desktop, including the ones it
     * doesn't belong to. Between cells the order doesn't matter: they
     * don't overlap.
     *
     * A wallpaper that *is* per desktop (xisback publishes a layer per
     * output and desktop) never reaches this code: it has a desktop of
     * its own, so it is an ordinary item in an ordinary cell, and each
     * cell shows its own picture. */
    {
        static CompSceneNode rebuilt[MAX_SCENE_NODES];
        int n = 0;

        /* A window that is not an item and is on screen now belongs to
         * whichever desktop this output is showing -- the one being
         * walked into, on the way out. That is the WM mapping a window
         * the compositor had no picture of (never seen mapped since it
         * started, so never stowed), or one opened while the grid was
         * up. It joins that desktop's cell as a late arrival, drawn in
         * its stack order among the cell's own windows and shrunk to the
         * cell as they are. Left where it fell -- at the bottom, whole,
         * untransformed -- it was drawn *under* the cells of the
         * desktops nobody chose, which are still fading out over it:
         * the desktop you are arriving at, painted over by the ones you
         * are leaving. */
        int late_cell = (d->closing && d->enter_desktop >= 0) ? d->enter_desktop
                                                              : d->current_desktop;

        /* Anything else that is not ours keeps its place at the bottom:
         * the WM's own overlays, and windows off this output. */
        for (int i = 0; i < s->count && n < MAX_SCENE_NODES; i++)
            if (!item_for(d, s->nodes[i].win) &&
                !late_arrival(o, &s->nodes[i]))
                rebuilt[n++] = s->nodes[i];

        /* Cell order, with one cell moved to the end: the desktop you are
         * going into is drawn over the others.
         *
         * On the way out this is what makes the effect read as walking
         * into that desktop rather than as four desktops growing into
         * each other -- they all end up filling the same screen, so
         * whichever is drawn last is the one that arrives in front, and
         * that has to be the one you chose. While the grid is up it is
         * the one under the pointer, which never overlaps anything
         * anyway. */
        int top_cell = d->closing ? moving_cell : d->selected;

        int order[MAX_DESKTOPS];
        int cells = 0;
        for (int c = 0; c < d->desktops && c < MAX_DESKTOPS; c++)
            if (c != top_cell)
                order[cells++] = c;
        if (top_cell >= 0 && top_cell < d->desktops && top_cell < MAX_DESKTOPS)
            order[cells++] = top_cell;

        for (int oi = 0; oi < cells; oi++) {
            int c = order[oi];
            /* Three passes over the original order, so the stacking
             * inside a cell is the stacking the WM gave those windows.
             * pass 0: the scenery that goes underneath; 1: the windows;
             * 2: the scenery on top. */
            for (int pass = 0; pass < 3; pass++) {
                for (int i = 0; i < s->count && n < MAX_SCENE_NODES; i++) {
                    ExItem *it = item_for(d, s->nodes[i].win);
                    if (!it) {
                        if (pass != 1 || c != late_cell ||
                            !late_arrival(o, &s->nodes[i]))
                            continue;
                        CompSceneNode node = s->nodes[i];
                        shrink_into_cell(d, o, &node, c, spread);
                        rebuilt[n++] = node;
                        continue;
                    }

                    /* Sticky scenery bookends the cell; everything that
                     * belongs to this desktop goes between, in the order
                     * the WM stacked it -- which already has a
                     * per-desktop wallpaper at the bottom, so it needs no
                     * special case here. */
                    bool want = it->sticky ? (pass == (it->below ? 0 : 2))
                                           : (pass == 1 && it->desktop == c);
                    if (!want)
                        continue;

                    CompSceneNode node = s->nodes[i];

                    if (it->sticky) {
                        /* This cell's copy moves with the cell: in place
                         * at full spread for a standing cell, along with
                         * the desktop for the moving one -- and fading
                         * with the leg where the cell stands, since a
                         * cell that does not move appears and goes by
                         * its opacity alone. */
                        float cs = (c == moving_cell) ? spread : 1.0f;
                        float ca = (c == moving_cell) ? 1.0f : spread;
                        CompRect target = place_in_cell(d, o, &it->home, c);
                        CompRect cur = lerp_rect(&it->home, &target, cs);

                        float sx = (float)cur.w /
                                   (float)(node.geometry.w > 0 ? node.geometry.w : 1);
                        float sy = (float)cur.h /
                                   (float)(node.geometry.h > 0 ? node.geometry.h : 1);

                        comp_transform_identity(&node.transform);
                        comp_transform_translate(&node.transform,
                                                 (float)-node.geometry.x,
                                                 (float)-node.geometry.y);
                        comp_transform_scale(&node.transform, sx, sy);
                        comp_transform_translate(&node.transform,
                                                 (float)cur.x, (float)cur.y);

                        node.opacity = it->alpha * ca;
                        if (d->selected >= 0 && c != d->selected) {
                            float dim = cfg->dim;
                            if (dim < 0.0f) dim = 0.0f;
                            if (dim > 1.0f) dim = 1.0f;
                            node.opacity *= comp_lerp(1.0f, dim, spread);
                        }

                        /* Clipped to its cell, for the same reason: the
                         * cell is this screen and a panel wider than the
                         * screen must not spill into the next cell. */
                        CompRect bound = clip_bound(&o->rect, &d->cell[c], cs);
                        CompRect vis;
                        if (!rect_intersect(&cur, &bound, &vis))
                            vis = (CompRect){ 0, 0, 0, 0 };
                        node.visible_rect = vis;
                    }

                    rebuilt[n++] = node;
                }
            }
        }

        if (n > 0) {
            memcpy(s->nodes, rebuilt, sizeof(CompSceneNode) * (size_t)n);
            s->count = n;
        }
    }

    /* The window you clicked travels in front of everything from the
     * first frame of the way out, rather than waiting for the WM to
     * raise it once the grid is already gone. Drawing order for these
     * frames only; the WM's stacking is not touched. */
    CompWindow *front = NULL;
    if (d->closing && d->enter_window)
        front = d->enter_window;
    else if (d->drag && d->drag_moving)
        front = d->drag->win;   /* carried over the other cells, not under them */
    if (front) {
        for (int i = 0; i < s->count; i++) {
            if (s->nodes[i].win != front)
                continue;
            CompSceneNode node = s->nodes[i];
            memmove(&s->nodes[i], &s->nodes[i + 1],
                    sizeof(CompSceneNode) * (size_t)(s->count - i - 1));
            s->nodes[s->count - 1] = node;
            break;
        }
    }
}

static bool ex_finished(const CompEffect *e, double now)
{
    const ExData *d = e->data;
    if (!d->closing)
        return false;
    return now >= d->leg_start + d->leg_ms;
}

static void ex_destroy(CompEffect *e)
{
    ExData *d = e->data;

    if (d && d->enter_window) {
        /* Focus last, so the WM's raise lands after the picture has
         * settled -- and it is a request, like everything else here. */
        CompWindow *w = d->enter_window;
        static xcb_atom_t net_active;
        if (!net_active) {
            xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(comp.conn,
                xcb_intern_atom(comp.conn, 0, 18, "_NET_ACTIVE_WINDOW"), NULL);
            if (r) {
                net_active = r->atom;
                free(r);
            }
        }
        if (net_active != XCB_NONE) {
            xcb_client_message_event_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.response_type = XCB_CLIENT_MESSAGE;
            msg.format = 32;
            msg.window = w->client != XCB_NONE ? w->client : w->id;
            msg.type = net_active;
            msg.data.data32[0] = 2;
            msg.data.data32[1] = XCB_CURRENT_TIME;
            xcb_send_event(comp.conn, 0, comp.root,
                           XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                           (const char *)&msg);
            xcb_flush(comp.conn);
        }
    }

    if (e == active) {
        active = NULL;
        comp.show_stowed_output = COMP_NO_OUTPUT;
        input_release();
    }
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps ex_ops = {
    .name       = "expo",
    .update     = ex_update,
    .apply      = ex_apply,
    .finished   = ex_finished,
    .destroy    = ex_destroy,
    .damage_map = ex_damage_map,
};

/* ------------------------------------------------------------------ */
/* the trigger                                                         */
/* ------------------------------------------------------------------ */

static void ex_toggle(void *data)
{
    const CompEffectInstance *self = data;

    if (active) {
        ExData *d = active->data;
        close_mode(active, d ? d->selected : -1, NULL);
        return;
    }

    int px = 0, py = 0;
    input_pointer_position(&px, &py);
    CompOutput *o = NULL;
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *c = &comp.outputs[i];
        if (px >= c->rect.x && px < c->rect.x + c->rect.w &&
            py >= c->rect.y && py < c->rect.y + c->rect.h)
            o = c;
    }
    if (!o)
        o = comp.output_count > 0 ? &comp.outputs[0] : NULL;
    if (!o)
        return;

    int desktops = desktop_count();
    if (desktops < 2)
        return;              /* one desktop is not a grid */
    if (desktops > MAX_DESKTOPS)
        desktops = MAX_DESKTOPS;

    CompEffect *e = calloc(1, sizeof(*e));
    ExData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->output_id = o->id;
    d->desktops = desktops;
    desktop_grid(&d->cols, &d->rows);
    if (d->cols < 1) d->cols = 1;
    if (d->rows < 1) d->rows = 1;
    d->current_desktop = desktop_current_for_output(o);
    d->selected = d->current_desktop >= 0 ? d->current_desktop : 0;
    d->enter_desktop = -1;

    int out_index = desktop_output_index(o);

    /* Every window this output's desktops hold: the ones on screen, and
     * the ones only a kept picture remains of (comp.h's keep_stowed). */
    for (CompWindow *w = comp.stack; w && d->count < MAX_ITEMS; w = w->next) {
        if (w->input_only || w->zombie || w->wm_layer[0])
            continue;
        if (!w->mapped && !w->stowed)
            continue;

        /* A minimized window is not on its desktop, and a cell is that
         * desktop as it would look. Its picture is kept (comp.h's
         * keep_stowed) because the minimize animation needs something to
         * shrink away, and for the effects that are *about* what is put
         * away -- but drawing it back onto the desktop it was taken off
         * shows a desktop nobody has. */
        if (w->state & COMP_STATE_MINIMIZED)
            continue;

        /* Scenery is in regardless of the type mask: the mask says which
         * windows the user is choosing between, and nobody is choosing
         * the wallpaper. */
        bool scenery = (w->type == COMP_WINDOW_DOCK ||
                        w->type == COMP_WINDOW_DESKTOP);
        if (!scenery && !(self->windows & COMP_WINDOW_BIT(w->type)))
            continue;

        int desk = -1, wout = -1;
        bool sticky = false;
        if (!desktop_of_window(w, &desk, &wout)) {
            /* A window the WM says nothing about. The wallpaper and the
             * panels are usually exactly that -- they are not on a
             * desktop, they are on all of them -- so scenery with no
             * answer is treated as everywhere and everything else is
             * left out. */
            if (!scenery)
                continue;
            sticky = true;
        }
        if (desk == COMP_DESKTOP_ALL)
            sticky = true;
        if (!sticky && (desk < 0 || desk >= desktops))
            continue;
        if (out_index >= 0 && wout >= 0 && wout != out_index)
            continue;

        CompRect r = window_rect(w);
        if (r.w <= 0 || r.h <= 0)
            continue;

        /* Scenery belongs to the screen it is on, and this grid is one
         * screen's. The other monitor's wallpaper and its panels are not
         * part of these desktops -- drawing them in these cells is the
         * other monitor's desktop appearing inside this one's. A window,
         * by contrast, is shown whole even where it hangs over the
         * boundary: it is a thing you are picking, not scenery. */
        if (scenery) {
            CompRect hit;
            if (!rect_intersect(&r, &o->rect, &hit))
                continue;
        }

        ExItem *it = &d->items[d->count++];
        it->win = w;
        it->desktop = sticky ? d->current_desktop : desk;
        it->scenery = scenery;
        it->below = (w->type == COMP_WINDOW_DESKTOP);
        it->sticky = sticky;
        it->home = it->from = it->current = it->to = r;
        it->stowed = w->stowed;
        it->alpha = it->alpha_from = it->alpha_to = 1.0f;
    }

    if (d->count == 0) {
        free(e);
        free(d);
        return;
    }

    e->ops = &ex_ops;
    e->instance = self;
    e->window = NULL;
    e->start_time = comp_now_ms();
    e->duration = 0.0;
    e->data = d;

    if (!input_grab(&ex_input, e)) {
        free(e);
        free(d);
        return;
    }

    active = e;
    /* And ask for a fresh look at the wallpapers being hidden, for the
     * next time: the pictures this grid is drawing were taken when the
     * compositor started, and a slideshow has moved on since
     * (desktop.h). */
    desktop_request_prime();
    /* From here this output's scene includes the windows whose picture
     * is merely being kept -- its other desktops (scene.c). Only this
     * one's: the monitor next to it is showing a desktop somebody is
     * still using, and its own put-away windows have no business being
     * drawn over it. */
    comp.show_stowed_output = o->id;
    effects_add(e);

    /* And it stays on the desktop that was showing: the user hasn't
     * chosen anything yet, and an expo that opens with a different
     * desktop selected than the one you were just using answers a
     * question nobody asked. The arrow keys move it; a click lands it
     * (on_button). */
    layout(e, comp_now_ms());
}

static void ex_init(const CompEffectInstance *self)
{
    const ExConfig *cfg = self->config;
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
            input_bind_hotkey(tok, ex_toggle, (void *)self);
    }
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static void ex_defaults(void *config)
{
    ExConfig *c = config;
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", "Meta+E");
    /* The same gap all round: between two cells and between a cell and
     * the edge of the screen. Different numbers there read as a mistake
     * rather than as a decision -- the grid looks off-centre even when
     * it is centred. Either can still be set on its own. */
    c->margin = 24;
    c->padding = 24;
    c->dim = 0.82f;
    c->arrange = ARRANGE_STACK;
    /* Black, and black on purpose: the point of the ground is that the
     * desktop you came from is *gone* while you choose, and any colour
     * with something in it reads as another desktop. */
    c->bg[0] = c->bg[1] = c->bg[2] = 0.0f;
    /* Nothing held: the other desktops show their last picture, and the
     * windows on them go on costing nothing, which is the whole point of
     * the WM having put them away. */
    c->live = LIVE_DESKTOP;
}

static bool ex_config_key(void *config, const char *key, const char *value)
{
    ExConfig *c = config;

    if (!strcmp(key, "hotkey")) {
        snprintf(c->hotkey, sizeof(c->hotkey), "%s", value);
        return true;
    }
    if (!strcmp(key, "margin")) {
        c->margin = atoi(value);
        return true;
    }
    if (!strcmp(key, "padding")) {
        c->padding = atoi(value);
        return true;
    }
    if (!strcmp(key, "dim")) {
        c->dim = (float)atof(value);
        return true;
    }
    if (!strcmp(key, "background")) {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf(value, " #%2x%2x%2x", &r, &g, &b) == 3 ||
            sscanf(value, " %2x%2x%2x", &r, &g, &b) == 3) {
            c->bg[0] = (float)r / 255.0f;
            c->bg[1] = (float)g / 255.0f;
            c->bg[2] = (float)b / 255.0f;
        } else {
            fprintf(stderr, "kicomp: config: expo: not a colour: '%s'\n", value);
        }
        return true;
    }
    if (!strcmp(key, "live_windows")) {
        if (!strcmp(value, "desktop"))          c->live = LIVE_DESKTOP;
        else if (!strcmp(value, "active_only")) c->live = LIVE_ACTIVE_ONLY;
        else if (!strcmp(value, "active"))      c->live = LIVE_ACTIVE;
        else if (!strcmp(value, "all"))         c->live = LIVE_ALL;
        else
            fprintf(stderr, "kicomp: config: unknown live_windows '%s'\n", value);
        return true;
    }
    if (!strcmp(key, "arrange")) {
        if (!strcmp(value, "grid"))
            c->arrange = ARRANGE_GRID;
        else if (!strcmp(value, "stack"))
            c->arrange = ARRANGE_STACK;
        else
            fprintf(stderr, "kicomp: config: unknown arrange '%s'\n", value);
        return true;
    }
    return false;
}

const CompEffectModule effect_expo = {
    .name             = "expo",
    .default_enabled  = true,
    .default_duration = 1.5,
    .default_easing   = COMP_EASE_OUT,
    .default_events   = 0,        /* its own hotkey, like show-windows */
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),

    .init             = ex_init,
    .config_size      = sizeof(ExConfig),
    .config_defaults  = ex_defaults,
    .config_key       = ex_config_key,
};
