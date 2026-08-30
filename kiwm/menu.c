/* menu.c - the window context menu (right-click a titlebar, or left-click
 * the window icon).
 *
 * Same look as everything else kiwm draws itself: an override-redirect
 * window with the decoration's own background/border colors and corner
 * radius, clipped with XCB SHAPE since there's no compositor, painted
 * off-screen and blitted in one go -- the pattern osd.c and
 * decoration.c already use. The row styling (hover wash, separator line,
 * disabled dimming, submenu arrow) mirrors xispanel's menu.c so the two
 * programs' menus read as the same widget.
 *
 * **Adding an action** is meant to be one row in `entries[]` below plus
 * one case in run_action(). An entry carries its label (and an optional
 * second label for when it's "on" -- Maximize/Restore, Shade/Unshade), an
 * optional predicate for whether it's selectable at all (kiwm already
 * knows what each window allows, see Client::allow_*), and an optional
 * predicate for its current state, drawn either as a check mark or as the
 * swapped label. Nothing else in this file needs to know the action
 * exists.
 *
 * Submenus are a stack of separate popup windows, one per open level,
 * each placed beside the row that spawned it -- classic menu behavior,
 * same as xispanel's. Only one level is actually used today ("Move to
 * desktop"), and MENU_MAX_FRAMES caps how deep that stack can go; the
 * hit-testing and drawing are per-frame either way, so a second level
 * would be a submenu kind and a builder, not a rework.
 *
 * The root popup takes an active pointer and keyboard grab for as long as
 * the menu is open (owner_events=0, so every event lands here regardless
 * of what's physically under the pointer, and hit-testing runs off
 * root-relative coordinates). A click outside every open frame just
 * dismisses the menu without being replayed to whatever was underneath --
 * the same accepted simplification xispanel's menu makes.
 */
#include "menu.h"
#include "client.h"
#include "decoration.h"
#include "keybind.h"
#include "output.h"

#include <cairo/cairo-xcb.h>
#include <xcb/shape.h>

#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_ROW_H        26
#define MENU_SEP_H         9
#define MENU_PAD_Y         4    /* chrome padding above the first row / below the last */
#define MENU_TEXT_X       10    /* left inset of a row's label */
#define MENU_CHECK_W      16    /* column reserved on the left for check marks */
#define MENU_ARROW_W      16    /* column reserved on the right for submenu arrows */
#define MENU_MIN_W       150
#define MENU_MAX_W       420
#define MENU_MAX_FRAMES    3
#define MENU_MAX_ITEMS   (MAX_DESKTOPS + 16)

/* ---- what the menu offers ---- */

typedef enum {
    ACT_NONE = 0,      /* a submenu parent: opens instead of doing something */
    ACT_MINIMIZE,
    ACT_MAXIMIZE,
    ACT_SHADE,
    ACT_STICKY,
    ACT_KEEP_ABOVE,
    ACT_CLOSE,
    ACT_TO_DESKTOP,    /* built at open time, one per desktop -- see build_desktop_frame() */
} MenuAct;

typedef enum { SUB_NONE = 0, SUB_DESKTOPS } SubmenuKind;

typedef struct {
    const char *label;
    /* Shown instead of `label` when on() reports true, for the entries
     * that are really one action in two directions. NULL means on() (if
     * any) draws a check mark instead. */
    const char *label_on;
    MenuAct action;
    SubmenuKind submenu;
    bool separator;
    bool (*allowed)(const Client *c);  /* NULL = always selectable */
    bool (*on)(const Client *c);       /* NULL = no state to show */
} MenuEntry;

static bool can_minimize(const Client *c)  { return c->allow_minimize; }
static bool can_maximize(const Client *c)  { return c->allow_maximize; }
static bool can_close(const Client *c)     { return c->allow_close; }
static bool can_pick_desktop(const Client *c) { return !c->sticky && wm.num_desktops > 1; }

static bool is_maximized(const Client *c)  { return client_maximized(c); }
static bool is_shaded(const Client *c)     { return c->shaded; }
static bool is_sticky(const Client *c)     { return c->sticky; }
static bool is_kept_above(const Client *c) { return c->keep_above; }

static const MenuEntry entries[] = {
    { .label = "Minimize",       .action = ACT_MINIMIZE,   .allowed = can_minimize },
    { .label = "Maximize",       .label_on = "Restore",
      .action = ACT_MAXIMIZE,    .allowed = can_maximize,  .on = is_maximized },
    { .label = "Shade",          .label_on = "Unshade",
      .action = ACT_SHADE,       .on = is_shaded },
    { .separator = true },
    { .label = "Move to desktop", .submenu = SUB_DESKTOPS, .allowed = can_pick_desktop },
    { .label = "All desktops",   .action = ACT_STICKY,     .on = is_sticky },
    { .label = "Keep above",     .action = ACT_KEEP_ABOVE, .on = is_kept_above },
    { .separator = true },
    { .label = "Close",          .action = ACT_CLOSE,      .allowed = can_close },
};
#define ENTRY_COUNT ((int)(sizeof(entries) / sizeof(entries[0])))

/* ---- open menu state ---- */

typedef struct {
    const char *label;   /* static text, or into desktop_labels[] */
    MenuAct action;
    int arg;             /* ACT_TO_DESKTOP: which desktop */
    SubmenuKind submenu;
    bool separator;
    bool enabled;
    bool checked;
    int y, h;            /* row rect within its frame's content area */
} MenuItem;

typedef struct {
    xcb_window_t win;
    int x, y, w, h;      /* on-screen rect, chrome included */
    MenuItem items[MENU_MAX_ITEMS];
    int count;
    int hover;           /* index into items, or -1 */
    int opened_by;       /* index in the *parent* frame of the row that opened this one, or -1 */
} MenuFrame;

static MenuFrame frames[MENU_MAX_FRAMES];
static int frame_count = 0;
static Client *menu_client = NULL;
static bool grabbed = false;

static char desktop_labels[MAX_DESKTOPS][32];

/* ---- theme ---- */

static void menu_colors(double *bg_r, double *bg_g, double *bg_b,
                        double *fg_r, double *fg_g, double *fg_b,
                        double *br_r, double *br_g, double *br_b)
{
    if (wm.have_theme_colors) {
        *bg_r = wm.bg_active_r; *bg_g = wm.bg_active_g; *bg_b = wm.bg_active_b;
        *fg_r = wm.fg_active_r; *fg_g = wm.fg_active_g; *fg_b = wm.fg_active_b;
        *br_r = wm.border_active_r; *br_g = wm.border_active_g; *br_b = wm.border_active_b;
    } else {
        *bg_r = wm.deco_bg_r; *bg_g = wm.deco_bg_g; *bg_b = wm.deco_bg_b;
        *fg_r = wm.deco_fg_r; *fg_g = wm.deco_fg_g; *fg_b = wm.deco_fg_b;
        *br_r = wm.deco_fg_r; *br_g = wm.deco_fg_g; *br_b = wm.deco_fg_b;
    }
}

static double menu_font_size(void)
{
    return wm.title_font_size + 1;
}

/* ---- building the item lists ---- */

static void add_item(MenuFrame *f, const MenuItem *item)
{
    if (f->count >= MENU_MAX_ITEMS)
        return;
    f->items[f->count++] = *item;
}

static void build_root_items(MenuFrame *f, const Client *c)
{
    f->count = 0;
    for (int i = 0; i < ENTRY_COUNT; i++) {
        const MenuEntry *e = &entries[i];
        bool on = e->on && e->on(c);
        MenuItem item = {
            .label = (on && e->label_on) ? e->label_on : e->label,
            .action = e->action,
            .arg = 0,
            .submenu = e->submenu,
            .separator = e->separator,
            .enabled = e->separator ? false : (!e->allowed || e->allowed(c)),
            /* A state predicate with no second label is a check mark: the
             * entry is a toggle that's either on or off right now. */
            .checked = on && !e->label_on,
        };
        add_item(f, &item);
    }
}

static void build_desktop_items(MenuFrame *f, const Client *c)
{
    f->count = 0;
    for (int d = 0; d < wm.num_desktops && d < MAX_DESKTOPS; d++) {
        snprintf(desktop_labels[d], sizeof(desktop_labels[d]), "Desktop %d", d + 1);
        MenuItem item = {
            .label = desktop_labels[d],
            .action = ACT_TO_DESKTOP,
            .arg = d,
            .enabled = true,
            .checked = (c->desktop == d),
        };
        add_item(f, &item);
    }
}

/* ---- geometry ---- */

/* Measures a frame's natural size and assigns every row its y/h, so both
 * painting and hit-testing read the same layout out of the items. */
static void layout_frame(MenuFrame *f)
{
    /* Text measuring needs a cairo context, and the popup window doesn't
     * exist yet at this point -- a 1x1 scratch surface is enough, since
     * only the font metrics matter. */
    cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(scratch);

    double widest = 0;
    bool any_submenu = false;
    for (int i = 0; i < f->count; i++) {
        if (f->items[i].separator)
            continue;
        double w = 0;
        pango_show_text_boxed(cr, 0, -1000, MENU_ROW_H, 0, menu_font_size(), f->items[i].label, false, &w);
        if (w > widest)
            widest = w;
        if (f->items[i].submenu != SUB_NONE)
            any_submenu = true;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(scratch);

    int w = MENU_TEXT_X + MENU_CHECK_W + (int)(widest + 0.5) + MENU_TEXT_X;
    if (any_submenu)
        w += MENU_ARROW_W;
    if (w < MENU_MIN_W) w = MENU_MIN_W;
    if (w > MENU_MAX_W) w = MENU_MAX_W;

    int y = MENU_PAD_Y;
    for (int i = 0; i < f->count; i++) {
        f->items[i].y = y;
        f->items[i].h = f->items[i].separator ? MENU_SEP_H : MENU_ROW_H;
        y += f->items[i].h;
    }

    f->w = w;
    f->h = y + MENU_PAD_Y;
}

/* Keeps a frame inside the output it lands on: menus grow down and to the
 * right from where they're anchored, and flip back over the anchor when
 * there's no room -- what every other menu does, and the only thing that
 * makes a right-click near the bottom edge usable at all. */
static void clamp_to_output(MenuFrame *f, int anchor_x, int anchor_y, int flip_w)
{
    int idx = output_index_for_point(f->x, f->y);
    if (idx < 0)
        idx = output_for_pointer();
    if (idx < 0 || idx >= wm.output_count)
        return;

    XisOutput *o = &wm.outputs[idx];
    if (f->x + f->w > o->x + o->width)
        f->x = anchor_x - f->w - flip_w;
    if (f->x < o->x)
        f->x = o->x;
    if (f->y + f->h > o->y + o->height)
        f->y = anchor_y - f->h;
    if (f->y < o->y)
        f->y = o->y;
}

/* ---- painting ---- */

static void paint_frame(MenuFrame *f)
{
    double bg_r, bg_g, bg_b, fg_r, fg_g, fg_b, br_r, br_g, br_b;
    menu_colors(&bg_r, &bg_g, &bg_b, &fg_r, &fg_g, &fg_b, &br_r, &br_g, &br_b);

    xcb_pixmap_t pixmap = xcb_generate_id(wm.conn);
    xcb_create_pixmap(wm.conn, wm.screen->root_depth, pixmap, f->win, (uint16_t)f->w, (uint16_t)f->h);
    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, pixmap, wm.visual, f->w, f->h);
    cairo_t *cr = cairo_create(surface);

    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, br_r, br_g, br_b);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, 0.75, 0.75, f->w - 1.5, f->h - 1.5);
    cairo_stroke(cr);

    for (int i = 0; i < f->count; i++) {
        MenuItem *it = &f->items[i];

        if (it->separator) {
            cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.15);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, 6, it->y + it->h / 2.0);
            cairo_line_to(cr, f->w - 6, it->y + it->h / 2.0);
            cairo_stroke(cr);
            continue;
        }

        if (i == f->hover && it->enabled) {
            cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.12);
            cairo_rectangle(cr, 1, it->y, f->w - 2, it->h);
            cairo_fill(cr);
        }

        double alpha = it->enabled ? 0.95 : 0.4;

        if (it->checked) {
            /* A plain two-stroke check mark, scaled off the row height so
             * it tracks the theme's font size like everything else. */
            double cx = MENU_TEXT_X + MENU_CHECK_W / 2.0;
            double cy = it->y + it->h / 2.0;
            cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, alpha);
            cairo_set_line_width(cr, 1.6);
            cairo_move_to(cr, cx - 4, cy);
            cairo_line_to(cr, cx - 1, cy + 3.5);
            cairo_line_to(cr, cx + 4.5, cy - 4);
            cairo_stroke(cr);
        }

        double text_x = MENU_TEXT_X + MENU_CHECK_W;
        double text_max = f->w - text_x - MENU_TEXT_X - (it->submenu != SUB_NONE ? MENU_ARROW_W : 0);
        cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, alpha);
        pango_show_text_boxed(cr, text_x, it->y, it->h, text_max, menu_font_size(),
                              it->label, false, NULL);

        if (it->submenu != SUB_NONE) {
            double ax = f->w - MENU_ARROW_W;
            double ay = it->y + it->h / 2.0;
            cairo_set_line_width(cr, 1.4);
            cairo_move_to(cr, ax, ay - 4);
            cairo_line_to(cr, ax + 5, ay);
            cairo_line_to(cr, ax, ay + 4);
            cairo_stroke(cr);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    xcb_copy_area(wm.conn, pixmap, f->win, wm.deco_gc, 0, 0, 0, 0, (uint16_t)f->w, (uint16_t)f->h);
    xcb_free_pixmap(wm.conn, pixmap);
}

static void shape_frame(MenuFrame *f)
{
    if (!wm.shape_ext_present)
        return;
    bool square = (wm.radius_tl == 0 && wm.radius_tr == 0 && wm.radius_br == 0 && wm.radius_bl == 0);
    if (square) {
        xcb_shape_mask(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, f->win, 0, 0, XCB_PIXMAP_NONE);
        return;
    }
    xcb_rectangle_t rects[2 * MAX_CORNER_RADIUS + 1];
    int n = build_rounded_rects(f->w, f->h, wm.radius_tl, wm.radius_tr, wm.radius_br, wm.radius_bl,
                                rects, (int)(sizeof(rects) / sizeof(rects[0])));
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_Y_SORTED,
                         f->win, 0, 0, (uint32_t)n, rects);
}

static void map_frame(MenuFrame *f)
{
    f->win = xcb_generate_id(wm.conn);
    uint32_t values[] = { wm.screen->black_pixel, 1, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(wm.conn, wm.screen->root_depth, f->win, wm.root,
                      (int16_t)f->x, (int16_t)f->y, (uint16_t)f->w, (uint16_t)f->h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    shape_frame(f);
    xcb_map_window(wm.conn, f->win);
    /* Mapping doesn't restack: one explicit raise, and from then on
     * restack_all() keeps it in the top layer via
     * window_menu_owns_window(). */
    xcb_configure_window(wm.conn, f->win, XCB_CONFIG_WINDOW_STACK_MODE,
                         (uint32_t[]){ XCB_STACK_MODE_ABOVE });
    paint_frame(f);
    xcb_flush(wm.conn);
}

static void destroy_frame(MenuFrame *f)
{
    if (f->win != XCB_NONE)
        xcb_destroy_window(wm.conn, f->win);
    memset(f, 0, sizeof(*f));
    f->hover = -1;
    f->opened_by = -1;
}

/* Closes every frame deeper than `depth` (0 = keep only the root). */
static void close_frames_below(int depth)
{
    while (frame_count > depth + 1)
        destroy_frame(&frames[--frame_count]);
}

/* ---- hit testing ---- */

static int frame_at(int root_x, int root_y)
{
    for (int i = frame_count - 1; i >= 0; i--) {
        MenuFrame *f = &frames[i];
        if (root_x >= f->x && root_x < f->x + f->w && root_y >= f->y && root_y < f->y + f->h)
            return i;
    }
    return -1;
}

static int item_at(const MenuFrame *f, int root_y)
{
    int local_y = root_y - f->y;
    for (int i = 0; i < f->count; i++)
        if (local_y >= f->items[i].y && local_y < f->items[i].y + f->items[i].h)
            return i;
    return -1;
}

/* ---- opening submenus / running actions ---- */

static void open_submenu(int parent_depth, int row)
{
    if (parent_depth + 1 >= MENU_MAX_FRAMES || !menu_client)
        return;

    MenuFrame *parent = &frames[parent_depth];
    if (row < 0 || row >= parent->count || parent->items[row].submenu == SUB_NONE ||
        !parent->items[row].enabled)
        return;

    /* Already showing this exact submenu -- leave it alone rather than
     * tearing it down and rebuilding it under the pointer. */
    if (frame_count > parent_depth + 1 && frames[parent_depth + 1].opened_by == row)
        return;

    close_frames_below(parent_depth);

    MenuFrame *f = &frames[parent_depth + 1];
    memset(f, 0, sizeof(*f));
    f->hover = -1;
    f->opened_by = row;

    switch (parent->items[row].submenu) {
    case SUB_DESKTOPS: build_desktop_items(f, menu_client); break;
    case SUB_NONE:     break;
    }
    if (f->count == 0)
        return;

    layout_frame(f);
    /* Beside the row that opened it, slightly overlapping the parent's
     * border so the two read as one connected menu. */
    f->x = parent->x + parent->w - 2;
    f->y = parent->y + parent->items[row].y - MENU_PAD_Y;
    clamp_to_output(f, parent->x + 2, f->y, parent->w - 4);

    frame_count = parent_depth + 2;
    map_frame(f);
}

/* Takes the client explicitly rather than reading menu_client: picking a
 * row closes the menu *first* (so the popup is gone before the action's
 * own repaints/restacks happen), and closing is what clears menu_client. */
static void run_action(Client *c, const MenuItem *it)
{
    if (!c || !it->enabled)
        return;

    switch (it->action) {
    case ACT_MINIMIZE:   minimize_client(c); break;
    case ACT_MAXIMIZE:   toggle_maximize(c, -1); break;
    case ACT_SHADE:      toggle_shade(c, -1); break;
    case ACT_STICKY:     toggle_sticky(c, -1); break;
    case ACT_KEEP_ABOVE: toggle_keep_above(c, -1); break;
    case ACT_CLOSE:      close_client(c); break;
    case ACT_TO_DESKTOP: set_client_desktop(c, it->arg); break;
    case ACT_NONE:       break;
    }
}

/* Picking a row: a submenu parent opens its submenu, anything else runs
 * and closes the whole menu. */
static void activate(int depth, int row)
{
    MenuFrame *f = &frames[depth];
    if (row < 0 || row >= f->count || f->items[row].separator || !f->items[row].enabled)
        return;

    if (f->items[row].submenu != SUB_NONE) {
        open_submenu(depth, row);
        return;
    }

    MenuItem picked = f->items[row];
    Client *c = menu_client;
    window_menu_close();
    run_action(c, &picked);
    xcb_flush(wm.conn);
}

/* ---- keyboard ---- */

/* The navigation keys a menu answers to, resolved once against the
 * server's keyboard mapping (keybind.c's keycode_for_keysym()) the first
 * time a menu opens rather than on every key press. Not configurable on
 * purpose: these are the fixed conventions of a menu widget, like
 * Escape's role in the switcher overlay, not user shortcuts. */
static struct {
    bool resolved;
    xcb_keycode_t up, down, left, right, enter, kp_enter, space;
} nav;

static void resolve_nav_keys(void)
{
    if (nav.resolved)
        return;
    nav.up       = keycode_for_keysym(XK_Up);
    nav.down     = keycode_for_keysym(XK_Down);
    nav.left     = keycode_for_keysym(XK_Left);
    nav.right    = keycode_for_keysym(XK_Right);
    nav.enter    = keycode_for_keysym(XK_Return);
    nav.kp_enter = keycode_for_keysym(XK_KP_Enter);
    nav.space    = keycode_for_keysym(XK_space);
    nav.resolved = true;
}

static void move_hover(int depth, int direction)
{
    MenuFrame *f = &frames[depth];
    int i = f->hover;
    for (int steps = 0; steps < f->count; steps++) {
        i = (i < 0 && direction < 0) ? f->count - 1 : (i + direction + f->count) % f->count;
        if (!f->items[i].separator && f->items[i].enabled) {
            f->hover = i;
            paint_frame(f);
            xcb_flush(wm.conn);
            return;
        }
    }
}

/* ---- public API ---- */

bool window_menu_active(void)
{
    return frame_count > 0;
}

bool window_menu_owns_window(xcb_window_t window)
{
    for (int i = 0; i < frame_count; i++)
        if (frames[i].win == window)
            return true;
    return false;
}

void window_menu_close(void)
{
    if (frame_count == 0)
        return;

    while (frame_count > 0)
        destroy_frame(&frames[--frame_count]);

    if (grabbed) {
        xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(wm.conn, XCB_CURRENT_TIME);
        grabbed = false;
    }
    menu_client = NULL;
    xcb_flush(wm.conn);
}

void window_menu_client_destroyed(Client *c)
{
    if (menu_client == c)
        window_menu_close();
}

void window_menu_open(Client *c, int root_x, int root_y)
{
    if (!c)
        return;

    window_menu_close();
    resolve_nav_keys();

    menu_client = c;

    MenuFrame *f = &frames[0];
    memset(f, 0, sizeof(*f));
    f->hover = -1;
    f->opened_by = -1;

    build_root_items(f, c);
    layout_frame(f);
    f->x = root_x;
    f->y = root_y;
    clamp_to_output(f, root_x, root_y, 0);

    frame_count = 1;
    map_frame(f);

    /* Async pointer grab: unlike osd.c's, nothing here ever needs to
     * replay an event to the window underneath (a click outside just
     * dismisses), so there's no reason to freeze the pointer. */
    xcb_grab_pointer_reply_t *ptr = xcb_grab_pointer_reply(wm.conn,
        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
    xcb_grab_keyboard_reply_t *kb = xcb_grab_keyboard_reply(wm.conn,
        xcb_grab_keyboard(wm.conn, 0, wm.root, XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    grabbed = (ptr && ptr->status == XCB_GRAB_STATUS_SUCCESS);
    free(ptr);
    free(kb);

    xcb_flush(wm.conn);
}

void window_menu_handle_expose(xcb_window_t window)
{
    for (int i = 0; i < frame_count; i++) {
        if (frames[i].win != window)
            continue;
        paint_frame(&frames[i]);
        xcb_flush(wm.conn);
        return;
    }
}

bool window_menu_handle_motion(xcb_motion_notify_event_t *ev)
{
    if (frame_count == 0)
        return false;

    int depth = frame_at(ev->root_x, ev->root_y);
    if (depth < 0)
        return true; /* pointer is off the menu -- keep it open, just don't hover anything */

    MenuFrame *f = &frames[depth];
    int row = item_at(f, ev->root_y);
    if (row == f->hover)
        return true;

    f->hover = row;
    paint_frame(f);

    /* Moving onto a different row of an ancestor frame retires whatever
     * deeper frames it spawned; moving onto a submenu parent opens it.
     * Immediately, with no hover delay: kiwm has no timer infrastructure
     * of its own and this menu is a handful of rows, not xispanel's
     * potentially huge lazy folder trees where the delay earns its keep. */
    if (row >= 0 && f->items[row].submenu != SUB_NONE && f->items[row].enabled)
        open_submenu(depth, row);
    else
        close_frames_below(depth);

    xcb_flush(wm.conn);
    return true;
}

bool window_menu_handle_button_press(xcb_button_press_event_t *ev)
{
    if (frame_count == 0)
        return false;

    int depth = frame_at(ev->root_x, ev->root_y);
    if (depth < 0) {
        window_menu_close();
        return true;
    }

    activate(depth, item_at(&frames[depth], ev->root_y));
    return true;
}

bool window_menu_handle_key_press(xcb_key_press_event_t *ev)
{
    if (frame_count == 0)
        return false;

    int depth = frame_count - 1;

    if (ev->detail == wm.key_escape) {
        if (depth > 0)
            close_frames_below(depth - 1);
        else
            window_menu_close();
        xcb_flush(wm.conn);
        return true;
    }

    if (ev->detail == nav.down)
        move_hover(depth, +1);
    else if (ev->detail == nav.up)
        move_hover(depth, -1);
    else if (ev->detail == nav.right) {
        if (frames[depth].hover >= 0)
            open_submenu(depth, frames[depth].hover);
    } else if (ev->detail == nav.left) {
        if (depth > 0)
            close_frames_below(depth - 1);
    } else if (ev->detail == nav.enter || ev->detail == nav.kp_enter || ev->detail == nav.space) {
        activate(depth, frames[depth].hover);
    }

    xcb_flush(wm.conn);
    return true;
}
