/*
 * show-windows: every window at once, laid out in a grid on the screen
 * the pointer is on, to pick one from.
 *
 * The other effects in this directory answer to something the WM did.
 * This one is a *mode*: it is triggered by its own hotkey, it holds the
 * keyboard and the pointer for as long as it is up, and it ends when the
 * user chooses a window (or gives up). That makes it the first effect
 * that takes input -- see input.h for why that lives outside this file
 * and why there can only ever be one such mode at a time.
 *
 * What it does *not* do is move any window. Every window stays exactly
 * where the WM put it, at the size the WM gave it, for the entire time
 * the grid is up: the grid is scene transforms and nothing else (section
 * 27). Which is also why it costs nothing to cancel -- there is nothing
 * to put back.
 *
 * kicomp.conf:
 *
 *   [effect:show-windows]
 *   enabled  = 1
 *   hotkey   = Meta+A, Meta+W    # one action, as many keys as you like
 *   duration = 1.5              # multiples of animation_duration
 *   easing   = out
 *   order    = stack            # stack | alpha | mru -- the order of the
 *                               # cells: as stacked, by title, or most
 *                               # recently used first
 *   dim      = 0.78             # opacity of the unselected windows (1 = none)
 *   margin   = 48               # gap around the grid, in pixels
 *   padding  = 16               # gap between cells
 *   other_outputs = 0           # windows from the other monitors too
 *   hide_docks    = 1           # panels fade out while the grid is up
 *   labels        = 1           # window names under the thumbnails, and
 *                               # the filter box at the top of the screen
 *   filter_debounce_ms = 100
 *
 * Typing filters the grid by window title; Escape clears the filter, and
 * clears the mode when there is nothing to clear. Left/Right/Up/Down move
 * the selection, Return activates it, and so does clicking one.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../input.h"
#include "../window.h"
#include "../transform.h"
#include "../text.h"
#include "../scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define MAX_ITEMS 64
#define MAX_FILTER 64

typedef struct {
    CompWindow *win;

    /* Where the window really is. Never changes while the grid is up --
     * it is both where the animation came from and where it goes back
     * to, and the WM's answer to "where is that window" all along. */
    CompRect home;

    CompRect from;      /* this leg of the animation */
    CompRect to;
    CompRect current;   /* interpolated, for hit-testing the pointer */

    char title[192];
    bool shown;         /* passes the filter */
    int cell;           /* position in the visible grid, -1 when filtered out */

    /* Opacity is animated on the same leg as the movement, which is what
     * makes filtering read as windows leaving and coming back rather
     * than as the grid blinking. */
    float alpha_from, alpha_to, alpha;

    /* The name, drawn under the thumbnail. Rendered when the grid opens
     * and kept until it closes -- a title that doesn't change costs a
     * composite per frame, not a text layout. */
    struct CompTextImage *label;

    /* A panel or a taskbar: never in the grid, and optionally not on
     * screen either while the grid is up. It is not something to pick,
     * and leaving it lying over the top of the layout is the one part of
     * the desktop the grid cannot arrange around. */
    bool is_dock;
} SwItem;

typedef struct {
    SwItem items[MAX_ITEMS];
    int count;

    int output_id;
    int cols, rows;

    int selected;       /* index into items, -1 for none */
    bool closing;
    bool activate_on_close;

    /* One leg of movement: everything animates from `from` to `to`
     * between these two instants, whether that is the grid opening, the
     * filter rearranging it, or the whole thing going home. */
    double leg_start;
    double leg_ms;

    /* Typing rearranges the grid, but not on every keystroke: a filter
     * that relayouts five times while you type a word is unreadable. */
    char filter[MAX_FILTER];
    bool filter_dirty;
    double filter_at;

    /* What the filter box currently reads. Re-rendered on each
     * keystroke, which is the one label that has to keep up with
     * typing. */
    struct CompTextImage *filter_image;
    char filter_shown[MAX_FILTER];
} SwData;

typedef enum {
    ORDER_STACK,   /* as they are stacked: the order the WM has them in */
    ORDER_ALPHA,   /* by title */
    ORDER_MRU,     /* most recently used first */
} SwOrder;

typedef struct {
    char hotkey[64];
    SwOrder order;
    float dim;
    int margin;
    int padding;
    bool other_outputs;
    bool hide_docks;
    bool labels;
    double debounce_ms;
} SwConfig;

static const CompEffectOps sw_ops;
static CompEffect *active;
static const CompEffectInstance *bound_instance;

/* ------------------------------------------------------------------ */
/* which windows, and what they are called                             */
/* ------------------------------------------------------------------ */

static xcb_atom_t atom(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(comp.conn,
        xcb_intern_atom(comp.conn, 0, (uint16_t)strlen(name), name), NULL);
    if (!r)
        return XCB_NONE;
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

/* _NET_WM_NAME first (UTF-8, what everything modern sets), WM_NAME as the
 * fallback. Read once when the grid opens: a title that changes while the
 * grid is up changes nothing the user can see. */
static void read_title(CompWindow *w, char *out, size_t outsz)
{
    static xcb_atom_t net_name, utf8;
    if (!net_name) {
        net_name = atom("_NET_WM_NAME");
        utf8 = atom("UTF8_STRING");
    }

    out[0] = '\0';
    xcb_window_t client = w->client != XCB_NONE ? w->client : w->id;

    for (int pass = 0; pass < 2; pass++) {
        xcb_atom_t prop = pass == 0 ? net_name : (xcb_atom_t)XCB_ATOM_WM_NAME;
        xcb_atom_t type = pass == 0 ? utf8 : (xcb_atom_t)XCB_ATOM_STRING;
        if (prop == XCB_NONE)
            continue;

        xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
            xcb_get_property(comp.conn, 0, client, prop, type, 0, 64), NULL);
        if (!r)
            continue;

        int len = xcb_get_property_value_length(r);
        if (len > 0) {
            if (len > (int)outsz - 1)
                len = (int)outsz - 1;
            memcpy(out, xcb_get_property_value(r), (size_t)len);
            out[len] = '\0';
        }
        free(r);
        if (out[0])
            return;
    }
}

/* Case-insensitive substring, which is what a filter box means by
 * "matches". ASCII folding only, deliberately: the alternative is a
 * Unicode case table for a convenience nobody will notice missing. */
static bool title_matches(const char *title, const char *needle)
{
    if (!needle[0])
        return true;
    size_t n = strlen(needle);
    for (const char *p = title; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

/* Which output a rectangle belongs to: the one containing its centre.
 * The centre rather than the corner, so a window straddling two monitors
 * lands on the one it is mostly on -- the same question the WM answers
 * the same way. */
static CompOutput *output_of(const CompRect *r)
{
    int cx = r->x + r->w / 2;
    int cy = r->y + r->h / 2;

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (cx >= o->rect.x && cx < o->rect.x + o->rect.w &&
            cy >= o->rect.y && cy < o->rect.y + o->rect.h)
            return o;
    }
    return NULL;
}

/* _NET_CLIENT_LIST: the WM's own answer to "which windows are windows".
 * Read once when the grid opens.
 *
 * This is the list a taskbar works from, and using the same one is the
 * point: a window nobody would expect to find in the taskbar -- a panel,
 * the desktop, an override-redirect popup, anything the WM never took on
 * -- is not something to pick out of a grid either. Reading the property
 * beats inferring it from window types, because it is the WM stating a
 * fact rather than us deducing one from what a client happened to
 * declare. */
static xcb_window_t *client_list;
static int client_list_len;

static void client_list_read(void)
{
    static xcb_atom_t net_client_list;
    if (!net_client_list)
        net_client_list = atom("_NET_CLIENT_LIST");

    free(client_list);
    client_list = NULL;
    client_list_len = 0;
    if (net_client_list == XCB_NONE)
        return;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, comp.root, net_client_list,
                         XCB_ATOM_WINDOW, 0, 4096), NULL);
    if (!r)
        return;

    int n = xcb_get_property_value_length(r) / 4;
    if (n > 0) {
        client_list = malloc(sizeof(xcb_window_t) * (size_t)n);
        if (client_list) {
            memcpy(client_list, xcb_get_property_value(r),
                   sizeof(xcb_window_t) * (size_t)n);
            client_list_len = n;
        }
    }
    free(r);
}

static bool in_client_list(const CompWindow *w)
{
    /* No list at all means no WM is publishing one; fall back to letting
     * the type mask decide rather than showing an empty grid. */
    if (client_list_len == 0)
        return true;

    xcb_window_t client = w->client != XCB_NONE ? w->client : w->id;
    for (int i = 0; i < client_list_len; i++)
        if (client_list[i] == client || client_list[i] == w->id)
            return true;
    return false;
}

/* And the other half of what a taskbar checks: a window that asked not to
 * be listed. An app that sets _NET_WM_STATE_SKIP_TASKBAR is saying it is
 * not one of its own windows to switch between. */
static bool skips_taskbar(const CompWindow *w)
{
    static xcb_atom_t net_state, skip;
    if (!net_state) {
        net_state = atom("_NET_WM_STATE");
        skip = atom("_NET_WM_STATE_SKIP_TASKBAR");
    }
    if (net_state == XCB_NONE || skip == XCB_NONE)
        return false;

    xcb_window_t client = w->client != XCB_NONE ? w->client : w->id;
    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, client, net_state, XCB_ATOM_ATOM, 0, 32),
        NULL);
    if (!r)
        return false;

    bool found = false;
    xcb_atom_t *atoms = xcb_get_property_value(r);
    int n = xcb_get_property_value_length(r) / 4;
    for (int i = 0; i < n && !found; i++)
        found = atoms[i] == skip;
    free(r);
    return found;
}

static bool eligible(const CompWindow *w, const SwConfig *cfg,
                     const CompEffectInstance *self, int output_id)
{
    if (!w->mapped || w->input_only || w->zombie)
        return false;
    if (!(self->windows & COMP_WINDOW_BIT(w->type)))
        return false;
    if (w->wm_layer[0])   /* kiwm's own OSD and outlines are not windows */
        return false;
    if (!in_client_list(w) || skips_taskbar(w))
        return false;

    /* Minimized windows, and windows on a desktop that isn't showing,
     * are unmapped -- there is no pixmap of them to draw, which is a
     * bigger problem than a filter and is why they are not here yet. */

    CompRect r = window_rect(w);
    if (r.w <= 0 || r.h <= 0)
        return false;

    if (!cfg->other_outputs) {
        CompOutput *o = output_of(&r);
        if (!o || o->id != output_id)
            return false;
    }
    return true;
}

/* The order the cells are filled in. Insertion sort on purpose: this is
 * one screen's worth of windows, sorted once when the grid opens.
 *
 * `stack` is the order the WM has them in, which is what the grid used to
 * be and is still the default -- it is the only order in which a window
 * you can see keeps the place your eye already knows. `alpha` is by
 * title. `mru` is most recently used first, the order the alt-tab OSD
 * offers, read from the compositor's own focus record (comp.h's
 * focus_serial) because X keeps no focus history to read. */
static void sort_items(SwData *d, SwOrder order)
{
    if (order == ORDER_STACK)
        return;

    for (int i = 1; i < d->count; i++) {
        SwItem item = d->items[i];
        int j = i - 1;
        while (j >= 0) {
            bool after;
            if (order == ORDER_ALPHA)
                after = strcasecmp(d->items[j].title, item.title) > 0;
            else
                after = d->items[j].win->focus_serial < item.win->focus_serial;
            if (!after)
                break;
            d->items[j + 1] = d->items[j];
            j--;
        }
        d->items[j + 1] = item;
    }
}

/* ------------------------------------------------------------------ */
/* the grid                                                            */
/* ------------------------------------------------------------------ */

/* A cell's worth of room for one window: as big as it can be inside the
 * cell without changing its proportions, and never bigger than the
 * window really is. Windows come *down* to the grid -- one blown up to
 * four times its size to fill a cell would look like a different window.
 */
static CompRect fit_in_cell(const CompRect *win, int cx, int cy, int cw, int ch)
{
    float sx = (float)cw / (float)(win->w > 0 ? win->w : 1);
    float sy = (float)ch / (float)(win->h > 0 ? win->h : 1);
    float s = sx < sy ? sx : sy;
    if (s > 1.0f)
        s = 1.0f;

    CompRect r;
    r.w = (int)((float)win->w * s + 0.5f);
    r.h = (int)((float)win->h * s + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = cx + (cw - r.w) / 2;
    r.y = cy + (ch - r.h) / 2;
    return r;
}

/* Lays the visible items out, and starts a leg of movement from wherever
 * each of them currently *looks* like it is -- so a filter that removes
 * half the grid slides the rest into their new places instead of
 * teleporting them. */
static void layout(CompEffect *e, double now)
{
    SwData *d = e->data;
    const SwConfig *cfg = e->instance->config;

    CompOutput *o = NULL;
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == d->output_id)
            o = &comp.outputs[i];
    if (!o)
        return;

    int shown = 0;
    for (int i = 0; i < d->count; i++) {
        d->items[i].cell = -1;
        if (d->items[i].shown)
            d->items[i].cell = shown++;
    }

    if (shown > 0) {
        /* As square as the count allows, then squared off against the
         * screen's proportions: on a wide monitor a 6-window grid reads
         * better as 3x2 than as 2x3. */
        d->cols = (int)ceil(sqrt((double)shown));
        if (o->rect.w > o->rect.h && d->cols * d->cols - shown >= d->cols)
            d->cols = (shown + 1) / 2 > 0 ? (shown + 1) / 2 : 1;
        if (d->cols < 1)
            d->cols = 1;
        d->rows = (shown + d->cols - 1) / d->cols;
    } else {
        d->cols = d->rows = 1;
    }

    int m = cfg->margin;
    int p = cfg->padding;
    int area_w = o->rect.w - m * 2;
    int area_h = o->rect.h - m * 2;
    if (area_w < 1) area_w = 1;
    if (area_h < 1) area_h = 1;

    int cw = (area_w - p * (d->cols - 1)) / d->cols;
    int ch = (area_h - p * (d->rows - 1)) / d->rows;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    for (int i = 0; i < d->count; i++) {
        SwItem *it = &d->items[i];
        it->from = it->current;

        it->alpha_from = it->alpha;

        if (it->is_dock) {
            it->to = it->home;              /* a panel never moves */
            continue;
        }

        if (it->cell < 0) {
            /* Filtered out: it fades where it stands rather than flying
             * home, because home is behind the grid and a window sliding
             * back into the pile reads as "that one got away" instead of
             * "that one doesn't match". */
            it->to = it->current;
            it->alpha_to = 0.0f;
            continue;
        }

        it->alpha_to = 1.0f;

        int col = it->cell % d->cols;
        int row = it->cell / d->cols;
        int cx = o->rect.x + m + col * (cw + p);
        int cy = o->rect.y + m + row * (ch + p);
        it->to = fit_in_cell(&it->home, cx, cy, cw, ch);
    }

    d->leg_start = now;
    d->leg_ms = effect_instance_duration(e->instance);
    output_damage_rect(&o->rect);
}

/* ------------------------------------------------------------------ */
/* selection                                                           */
/* ------------------------------------------------------------------ */

static int item_at_cell(SwData *d, int cell)
{
    for (int i = 0; i < d->count; i++)
        if (d->items[i].cell == cell)
            return i;
    return -1;
}

static void select_cell(SwData *d, int cell)
{
    int i = item_at_cell(d, cell);
    if (i < 0 || i == d->selected)
        return;
    d->selected = i;

    for (int k = 0; k < comp.output_count; k++)
        if (comp.outputs[k].id == d->output_id)
            output_damage_rect(&comp.outputs[k].rect);
}

static void select_first(SwData *d)
{
    d->selected = -1;
    select_cell(d, 0);
}

static void move_selection(SwData *d, int dx, int dy)
{
    if (d->selected < 0 || d->cols < 1)
        return;
    int cell = d->items[d->selected].cell;
    if (cell < 0)
        return;

    int col = cell % d->cols + dx;
    int row = cell / d->cols + dy;
    if (col < 0 || col >= d->cols || row < 0)
        return;

    int want = row * d->cols + col;
    if (item_at_cell(d, want) < 0)
        return;
    select_cell(d, want);
}

/* ------------------------------------------------------------------ */
/* opening and closing                                                 */
/* ------------------------------------------------------------------ */

static void close_mode(CompEffect *e, bool activate)
{
    SwData *d = e->data;
    if (d->closing)
        return;

    d->closing = true;
    d->activate_on_close = activate;
    input_release();

    for (int i = 0; i < d->count; i++) {
        d->items[i].from = d->items[i].current;
        d->items[i].to = d->items[i].home;
        d->items[i].alpha_from = d->items[i].alpha;
        /* Back to being themselves: the ones the filter hid fade in on
         * the way home, and the panels come back with them. */
        d->items[i].alpha_to = 1.0f;
    }
    d->leg_start = comp_now_ms();
    d->leg_ms = effect_instance_duration(e->instance);

    for (int k = 0; k < comp.output_count; k++)
        if (comp.outputs[k].id == d->output_id)
            output_damage_rect(&comp.outputs[k].rect);
}

/* Chosen: ask the WM to bring it forward, the way a pager does. The
 * window is not touched here -- which desktop it is on, whether it needs
 * raising, what takes focus, are all the WM's to decide (section 27). */
static void activate_selected(SwData *d)
{
    if (d->selected < 0)
        return;
    CompWindow *w = d->items[d->selected].win;
    if (!w)
        return;

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
    msg.data.data32[0] = 2;                  /* a pager, not the app itself */
    msg.data.data32[1] = XCB_CURRENT_TIME;

    xcb_send_event(comp.conn, 0, comp.root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&msg);
    xcb_flush(comp.conn);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static bool on_key(void *data, xcb_keysym_t sym, const char *text, uint16_t mods)
{
    CompEffect *e = data;
    SwData *d = e->data;
    (void)mods;

    switch (sym) {
    case 0xff1b:                     /* Escape */
        if (d->filter[0]) {
            d->filter[0] = '\0';
            d->filter_dirty = true;
            d->filter_at = comp_now_ms();
            return true;
        }
        close_mode(e, false);
        return true;
    case 0xff0d:                     /* Return */
    case 0xff8d:                     /* KP_Enter */
        close_mode(e, d->selected >= 0);
        return true;
    case 0xff08: {                   /* BackSpace */
        size_t n = strlen(d->filter);
        if (n > 0) {
            d->filter[n - 1] = '\0';
            d->filter_dirty = true;
            d->filter_at = comp_now_ms();
        }
        return true;
    }
    case 0xff51: move_selection(d, -1, 0); return true;   /* Left */
    case 0xff53: move_selection(d, +1, 0); return true;   /* Right */
    case 0xff52: move_selection(d, 0, -1); return true;   /* Up */
    case 0xff54: move_selection(d, 0, +1); return true;   /* Down */
    case 0xff09:                                          /* Tab */
        if (d->selected >= 0)
            select_cell(d, d->items[d->selected].cell + 1);
        return true;
    default:
        break;
    }

    if (text && text[0]) {
        size_t n = strlen(d->filter);
        size_t add = strlen(text);
        if (n + add < sizeof(d->filter)) {
            memcpy(d->filter + n, text, add + 1);
            d->filter_dirty = true;
            d->filter_at = comp_now_ms();
        }
        return true;
    }
    return false;
}

static void on_motion(void *data, int root_x, int root_y)
{
    CompEffect *e = data;
    SwData *d = e->data;

    for (int i = 0; i < d->count; i++) {
        SwItem *it = &d->items[i];
        if (it->cell < 0)
            continue;
        if (root_x >= it->current.x && root_x < it->current.x + it->current.w &&
            root_y >= it->current.y && root_y < it->current.y + it->current.h) {
            select_cell(d, it->cell);
            return;
        }
    }
}

static void on_button(void *data, int root_x, int root_y, uint8_t button,
                      bool pressed)
{
    CompEffect *e = data;
    SwData *d = e->data;

    /* The press only moves the selection under the pointer; letting go is
     * what chooses. Someone who presses on the wrong window can slide off
     * it and release somewhere else, which is how every button on every
     * desktop behaves. */
    if (pressed) {
        on_motion(data, root_x, root_y);
        return;
    }

    if (button != 1) {
        close_mode(e, false);
        return;
    }
    on_motion(data, root_x, root_y);
    /* Clicking the space between cells is not a choice -- it is how you
     * dismiss a grid, the same as clicking outside a menu. */
    bool on_a_window = d->selected >= 0 &&
        root_x >= d->items[d->selected].current.x &&
        root_x < d->items[d->selected].current.x + d->items[d->selected].current.w &&
        root_y >= d->items[d->selected].current.y &&
        root_y < d->items[d->selected].current.y + d->items[d->selected].current.h;
    close_mode(e, on_a_window);
}

static const CompInputHandler sw_input = {
    .key = on_key,
    .motion = on_motion,
    .button = on_button,
};

/* ------------------------------------------------------------------ */
/* the effect proper                                                   */
/* ------------------------------------------------------------------ */

static float leg_progress(const SwData *d, const CompEffect *e, double now)
{
    if (d->leg_ms <= 0.0)
        return 1.0f;
    return effect_ease(e, comp_progress(now, d->leg_start, d->leg_ms));
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

/* The text of the filter box: what has been typed, or an invitation when
 * nothing has. Re-rendered only when the string actually changed --
 * every keystroke, which is a layout per keystroke and no more. */
static void filter_image_update(SwData *d, const SwConfig *cfg)
{
    if (!cfg->labels)
        return;
    if (d->filter_image && !strcmp(d->filter_shown, d->filter))
        return;

    /* It changed, so the screen has to: typing is the one thing here
     * that alters the picture without anything moving, and the frame
     * clock only wakes for damage. */
    for (int k = 0; k < comp.output_count; k++)
        if (comp.outputs[k].id == d->output_id)
            output_damage_rect(&comp.outputs[k].rect);

    text_free(d->filter_image);
    snprintf(d->filter_shown, sizeof(d->filter_shown), "%s", d->filter);

    char shown[MAX_FILTER + 32];
    snprintf(shown, sizeof(shown), "%s", d->filter[0] ? d->filter : "type to filter");
    d->filter_image = text_render(shown, text_theme_style(), 0);
}

static void sw_update(CompEffect *e, double now)
{
    SwData *d = e->data;
    const SwConfig *cfg = e->instance->config;

    filter_image_update(d, cfg);

    /* The debounce: the filter is applied once typing pauses, not once
     * per keystroke, so a grid does not reshuffle under the fingers. */
    if (d->filter_dirty && now - d->filter_at >= cfg->debounce_ms) {
        d->filter_dirty = false;

        for (int i = 0; i < d->count; i++)
            d->items[i].shown = !d->items[i].is_dock &&
                                title_matches(d->items[i].title, d->filter);

        layout(e, now);

        /* Whatever was selected may have just been filtered away. */
        if (d->selected >= 0 && d->items[d->selected].cell < 0)
            select_first(d);
        else if (d->selected < 0)
            select_first(d);
    }

    float p = leg_progress(d, e, now);
    bool moving = p < 1.0f;

    for (int i = 0; i < d->count; i++) {
        d->items[i].current = lerp_rect(&d->items[i].from, &d->items[i].to, p);
        d->items[i].alpha = comp_lerp(d->items[i].alpha_from,
                                      d->items[i].alpha_to, p);
    }

    /* Only while something is actually moving: a grid sitting still is a
     * still picture, and repainting it sixty times a second for nothing
     * is exactly the CPU cost this compositor is careful about. */
    if (moving) {
        /* The grid's own screen, all of it -- every cell is in play.
         *
         * And then each window's real rectangle and its current one,
         * which is what repaints the *other* monitors: the strip of a
         * window hanging over the boundary is fading there, and with
         * other_outputs on a window is travelling across it. This is the
         * half the desktop-wall effect already gets right and the reason
         * its crossing fade looks clean -- a piece nobody damages is a
         * piece nobody repaints, so the fade only advances when
         * something else happens to dirty that ground.
         *
         * Rectangles rather than "every output", so a second monitor
         * with nothing happening on it is not repainted sixty times a
         * second for someone else's animation. */
        for (int k = 0; k < comp.output_count; k++)
            if (comp.outputs[k].id == d->output_id)
                output_damage_rect(&comp.outputs[k].rect);

        for (int i = 0; i < d->count; i++) {
            output_damage_rect(&d->items[i].home);
            output_damage_rect(&d->items[i].current);
        }
    }
}

static SwItem *item_for(SwData *d, const CompWindow *win)
{
    for (int k = 0; k < d->count; k++)
        if (d->items[k].win == win)
            return &d->items[k];
    return NULL;
}

static void sw_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    SwData *d = e->data;
    const SwConfig *cfg = e->instance->config;

    float p = leg_progress(d, e, comp_now_ms());
    float spread = d->closing ? 1.0f - p : p;   /* how "open" the grid is */

    /* The other monitors. A window whose place is on the grid's screen
     * can still hang over the edge onto a neighbour, and that strip has
     * nowhere to go: the grid is one screen's worth of layout. So it
     * fades out where it is and comes back when the grid closes, rather
     * than being left behind as a sliver of a window that is visibly
     * somewhere else now.
     *
     * Unless the grid was told to gather every screen's windows, in
     * which case there is no leftover to explain -- the window really is
     * travelling to the other monitor, and it should be seen doing it,
     * so it gets the same treatment here as anywhere else and the
     * transform carries it across the boundary. */
    if (o->id != d->output_id && !cfg->other_outputs) {
        for (int i = 0; i < s->count; i++) {
            SwItem *it = item_for(d, s->nodes[i].win);
            if (it)
                s->nodes[i].opacity *= comp_lerp(1.0f, 0.0f, spread);
        }
        return;
    }

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];

        SwItem *it = item_for(d, n->win);
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

        /* Dimmed unless it is the one under the cursor or the keyboard:
         * with no chrome to draw yet, this is what says which window
         * Return would pick. */
        n->opacity *= it->alpha;

        bool chosen = (d->selected >= 0 && &d->items[d->selected] == it);
        if (!chosen && !it->is_dock) {
            float dim = cfg->dim;
            if (dim < 0.0f) dim = 0.0f;
            if (dim > 1.0f) dim = 1.0f;
            n->opacity *= comp_lerp(1.0f, dim, spread);
        }

        CompRect vis;
        if (rect_intersect(cur, &o->rect, &vis))
            n->visible_rect = vis;
        else
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
    }

    /* The labels, once the grid is actually a grid: the name under each
     * thumbnail, and the filter box at the top of the screen.
     *
     * Faded in with the grid rather than appearing at once, and left out
     * entirely while a window is not in a cell -- a name under a window
     * that is on its way home belongs to nothing. */
    if (cfg->labels && spread > 0.02f) {
        for (int i = 0; i < d->count; i++) {
            SwItem *it = &d->items[i];
            if (!it->label || it->cell < 0 || it->is_dock)
                continue;

            int lw = text_width(it->label), lh = text_height(it->label);
            CompRect at = {
                it->current.x + (it->current.w - lw) / 2,
                it->current.y + it->current.h - lh / 2,
                lw, lh
            };
            float a = spread * it->alpha;
            /* The chosen one at full strength, the rest as dim as their
             * windows: the label is part of the window, not a separate
             * thing to read. */
            bool chosen = (d->selected >= 0 && &d->items[d->selected] == it);
            if (!chosen)
                a *= comp_lerp(1.0f, cfg->dim, spread);
            scene_add_chrome(s, it->label, &at, a);
        }

        if (d->filter_image) {
            int lw = text_width(d->filter_image), lh = text_height(d->filter_image);
            CompRect at = {
                o->rect.x + (o->rect.w - lw) / 2,
                o->rect.y + cfg->margin / 2,
                lw, lh
            };
            scene_add_chrome(s, d->filter_image, &at,
                             spread * (d->filter[0] ? 1.0f : 0.6f));
        }
    }

    /* On the way out, the window that was chosen travels home in front.
     *
     * The WM will raise it -- that is what _NET_ACTIVE_WINDOW asks for --
     * but not until the grid is gone and the message has made the round
     * trip, so without this the window you picked spends its whole
     * journey sliding *behind* the ones you didn't. Reordering the scene
     * changes the order this one frame is drawn in and nothing else: the
     * stacking the WM keeps is untouched, which is the rule every effect
     * here works under. */
    if (o->id == d->output_id && d->closing && d->activate_on_close &&
        d->selected >= 0) {
        CompWindow *pick = d->items[d->selected].win;
        for (int i = 0; i < s->count; i++) {
            if (s->nodes[i].win != pick)
                continue;
            CompSceneNode node = s->nodes[i];
            memmove(&s->nodes[i], &s->nodes[i + 1],
                    sizeof(CompSceneNode) * (size_t)(s->count - i - 1));
            s->nodes[s->count - 1] = node;
            break;
        }
    }
}

static bool sw_finished(const CompEffect *e, double now)
{
    const SwData *d = e->data;
    if (!d->closing)
        return false;
    return now >= d->leg_start + d->leg_ms;
}

static void sw_destroy(CompEffect *e)
{
    SwData *d = e->data;

    if (d && d->activate_on_close)
        activate_selected(d);

    for (int i = 0; d && i < d->count; i++)
        text_free(d->items[i].label);
    if (d)
        text_free(d->filter_image);

    if (e == active) {
        active = NULL;
        input_release();
    }
    free(client_list);
    client_list = NULL;
    client_list_len = 0;
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps sw_ops = {
    .name     = "show-windows",
    .update   = sw_update,
    .apply    = sw_apply,
    .finished = sw_finished,
    .destroy  = sw_destroy,
};

/* ------------------------------------------------------------------ */
/* the trigger                                                         */
/* ------------------------------------------------------------------ */

static void sw_toggle(void *data)
{
    const CompEffectInstance *self = data;
    const SwConfig *cfg = self->config;

    /* Already up: the hotkey is a toggle, which is what a key that opens
     * a mode has to be -- there is no second key to close it with. */
    if (active) {
        close_mode(active, active->data && ((SwData *)active->data)->selected >= 0);
        return;
    }

    /* The active screen is the one the pointer is on, decided once, when
     * the grid opens. */
    int px = 0, py = 0;
    input_pointer_position(&px, &py);
    CompRect at = { px, py, 1, 1 };
    CompOutput *o = output_of(&at);
    if (!o)
        o = comp.output_count > 0 ? &comp.outputs[0] : NULL;
    if (!o)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    SwData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->output_id = o->id;
    d->selected = -1;

    client_list_read();

    for (CompWindow *w = comp.stack; w && d->count < MAX_ITEMS; w = w->next) {
        if (!eligible(w, cfg, self, d->output_id))
            continue;

        SwItem *it = &d->items[d->count++];
        it->win = w;
        it->home = window_rect(w);
        it->from = it->home;
        it->current = it->home;
        it->to = it->home;
        it->shown = true;
        it->cell = -1;
        it->alpha = it->alpha_from = it->alpha_to = 1.0f;
        read_title(w, it->title, sizeof(it->title));
        if (cfg->labels)
            it->label = text_render(it->title, text_theme_style(), 320);
    }

    /* And the panels, if they are to get out of the way. They are items
     * like any other so that one leg of animation covers everything on
     * screen, but they never take a cell and never take the selection. */
    if (cfg->hide_docks) {
        for (CompWindow *w = comp.stack; w && d->count < MAX_ITEMS; w = w->next) {
            if (!w->mapped || w->input_only || w->zombie || w->wm_layer[0])
                continue;
            if (w->type != COMP_WINDOW_DOCK)
                continue;

            CompRect r = window_rect(w);
            if (r.w <= 0 || r.h <= 0)
                continue;
            CompOutput *ow = output_of(&r);
            if (!ow || ow->id != d->output_id)
                continue;

            SwItem *it = &d->items[d->count++];
            it->win = w;
            it->home = it->from = it->current = it->to = r;
            it->shown = false;
            it->cell = -1;
            it->is_dock = true;
            it->alpha = it->alpha_from = 1.0f;
            it->alpha_to = 0.0f;
        }
    }

    if (d->count == 0) {
        free(e);
        free(d);
        return;
    }

    sort_items(d, cfg->order);

    e->ops = &sw_ops;
    e->instance = self;
    e->window = NULL;         /* a mode, not a window's animation */
    e->start_time = comp_now_ms();
    e->duration = 0.0;        /* finished() decides, not a stopwatch */
    e->data = d;

    if (!input_grab(&sw_input, e)) {
        free(e);
        free(d);
        return;
    }

    active = e;
    effects_add(e);

    layout(e, comp_now_ms());
    select_first(d);
    on_motion(e, px, py);     /* whatever is already under the pointer */
}

static void sw_init(const CompEffectInstance *self)
{
    const SwConfig *cfg = self->config;
    if (!cfg->hotkey[0])
        return;

    bound_instance = self;

    /* A list, not one key: the same action reached from more than one
     * combination is an ordinary thing to want, and the alternative --
     * one binding, take it or leave it -- makes a user choose between
     * the key they are used to and the key they are moving to. */
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
            input_bind_hotkey(tok, sw_toggle, (void *)self);
    }
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static void sw_defaults(void *config)
{
    SwConfig *c = config;
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", "Meta+A, Meta+W");
    c->dim = 0.78f;
    c->margin = 48;
    c->padding = 16;
    c->order = ORDER_STACK;
    c->other_outputs = false;
    c->hide_docks = true;
    c->labels = true;
    c->debounce_ms = 100.0;
}

static bool sw_config_key(void *config, const char *key, const char *value)
{
    SwConfig *c = config;

    if (!strcmp(key, "hotkey")) {
        snprintf(c->hotkey, sizeof(c->hotkey), "%s", value);
        return true;
    }
    if (!strcmp(key, "order")) {
        if (!strcmp(value, "stack"))
            c->order = ORDER_STACK;
        else if (!strcmp(value, "alpha") || !strcmp(value, "alphabetical"))
            c->order = ORDER_ALPHA;
        else if (!strcmp(value, "mru") || !strcmp(value, "recent"))
            c->order = ORDER_MRU;
        else
            fprintf(stderr, "kicomp: config: unknown order '%s'\n", value);
        return true;
    }
    if (!strcmp(key, "dim")) {
        c->dim = (float)atof(value);
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
    if (!strcmp(key, "other_outputs")) {
        c->other_outputs = atoi(value) != 0;
        return true;
    }
    if (!strcmp(key, "hide_docks")) {
        c->hide_docks = atoi(value) != 0;
        return true;
    }
    if (!strcmp(key, "labels")) {
        c->labels = atoi(value) != 0;
        return true;
    }
    if (!strcmp(key, "filter_debounce_ms")) {
        c->debounce_ms = atof(value);
        return true;
    }
    return false;
}

const CompEffectModule effect_show_windows = {
    .name             = "show-windows",
    .default_enabled  = true,
    /* Longer than an ordinary transition: every window on the screen is
     * moving at once, and this is the effect the user is meant to watch
     * rather than not notice. */
    .default_duration = 1.5,
    .default_easing   = COMP_EASE_OUT,
    /* No events at all: this one is started by its own hotkey, not by
     * anything the WM does. */
    .default_events   = 0,
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),

    .init             = sw_init,
    .config_size      = sizeof(SwConfig),
    .config_defaults  = sw_defaults,
    .config_key       = sw_config_key,
};
