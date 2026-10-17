/*
 * xispanel_widget.h - what an installable panel widget (<type>.so) is, and
 * everything it may ask of xispanel.
 *
 * Self-contained (cairo only, none of xispanel's own structs) so it can be
 * installed and built against outside this tree. A module is one shared
 * object named after the widget type it provides -- kiremote.so answers a
 * `WIDGET <panel> <order> kiremote key=value ...` line -- found in the
 * directories PROTOCOL.md's "Widget modules" section lists. xispanel only
 * lists them at startup; the .so is dlopen()ed the first time a panel
 * actually uses the type. Build with -fPIC -shared and export:
 *
 *     const XispanelWidgetModule *xispanel_widget_entry(void);
 *
 * returning a static descriptor whose abi is XISPANEL_WIDGET_ABI; a module
 * built for another ABI is refused. Bump XISPANEL_WIDGET_ABI on any
 * incompatible change to this header.
 *
 * Modules run inside xispanel's process and its single event loop: never
 * block (use xpw_async() for anything that waits on another program), and
 * a crash in a module takes the panel down with it. The functions below
 * are resolved against the running xispanel binary (see widgets-api.sym --
 * only what is declared here is exported to modules).
 */
#ifndef XISPANEL_WIDGET_H
#define XISPANEL_WIDGET_H

#include <cairo/cairo.h>
#include <stddef.h>
#include <stdint.h>

#define XISPANEL_WIDGET_ABI 1
#define XISPANEL_WIDGET_ENTRY "xispanel_widget_entry"

/* One instance of the widget on one panel. Opaque; every callback gets it
 * and every xpw_*() call below takes it. */
typedef struct XpwWidget XpwWidget;

typedef struct {
    int abi; /* XISPANEL_WIDGET_ABI */
    /* Per-instance state, zeroed when the widget is created and freed with
     * it; xpw_priv() returns it. 0 = none. */
    size_t priv_size;
    /* 1 if the widget may sit inside a `container` popup (it then draws
     * and clicks exactly the same). */
    int embeddable;

    /* All optional. */
    int (*init)(XpwWidget *w);
    void (*destroy)(XpwWidget *w);
    /* Desired length along the panel's main axis, and the least it can
     * still be useful at. cross_axis is the panel's thickness. Default:
     * a square (len = cross_axis). */
    void (*measure)(XpwWidget *w, int cross_axis, int *out_len, int *out_min_len);
    /* Draw into the rectangle xpw_geometry() gives, in the panel window's
     * coordinates. Call xpw_paint_hover_bg() first for the usual hover
     * highlight. */
    void (*paint)(XpwWidget *w, cairo_t *cr);
    /* button is X11's: 1 left, 2 middle, 3 right, 4/5 wheel up/down.
     * Return 1 if the click was used. */
    int (*on_button)(XpwWidget *w, int button, int local_x, int local_y);
    /* Called once the time set with xpw_tick_at() comes (never before it
     * is set). Return 1 only if what paint() draws changed -- or its
     * measure() length did -- since the last tick, 0 otherwise: 1 repaints
     * the whole bar. */
    int (*on_tick)(XpwWidget *w, uint64_t now_ms);
    /* 1 while the widget has something worth showing on the bar even from
     * inside a container (WIDGET inline=urgent). */
    int (*is_urgent)(XpwWidget *w);
    /* Fill buf with the hover tooltip (lines separated by '\n'); return 1,
     * or 0 for no tooltip. */
    int (*get_tooltip)(XpwWidget *w, char *buf, size_t bufsz);
} XispanelWidgetModule;

typedef const XispanelWidgetModule *(*XispanelWidgetEntryFn)(void);

/* ---- instance ---- */

/* The priv_size bytes of this instance. */
void *xpw_priv(XpwWidget *w);
/* The widget line's own key=value option (WIDGET ... key=value), as kv_get
 * reads them: 1 and *out filled if present, else 0. Quote a value with
 * spaces: key="a b". */
int xpw_option(XpwWidget *w, const char *key, char *out, size_t outsz);
int xpw_option_int(XpwWidget *w, const char *key, int defval);
/* Where the widget is, in the panel window's coordinates (right for
 * paint(); already accounts for a vertical panel), and the size of its
 * cross axis -- the panel's thickness. */
void xpw_geometry(XpwWidget *w, int *x, int *y, int *width, int *height);
/* 1 if the panel is a vertical (left/right) one. */
int xpw_is_vertical(XpwWidget *w);
/* The panel's text/foreground and background colors, 0..1 RGBA. */
void xpw_colors(XpwWidget *w, double fg[4], double bg[4]);
/* Request the next on_tick() at this xpw_now_ms() time; 0 stops ticking. */
void xpw_tick_at(XpwWidget *w, uint64_t at_ms);
/* Repaint the panel at the next opportunity (on_tick() returning 1 is the
 * usual way; this is for after a click). */
void xpw_redraw(XpwWidget *w);
uint64_t xpw_now_ms(void);

/* ---- drawing ---- */

/* The panel's own hover highlight behind the widget, when the pointer is
 * over it. */
void xpw_paint_hover_bg(XpwWidget *w, cairo_t *cr);
/* The panel's text size in px, and the width `text` takes at it (usable
 * from measure()). */
double xpw_text_size(XpwWidget *w);
double xpw_text_width(XpwWidget *w, const char *text);
/* `text` at the panel's font, vertically centred in a box_h-high band
 * starting at top_y, in the current source color. */
void xpw_draw_text(XpwWidget *w, cairo_t *cr, double x, double top_y, double box_h, const char *text);
/* An icon by theme name or absolute path, shrunk to fit px x px; NULL if
 * not found. The caller owns it: cairo_surface_destroy(). */
cairo_surface_t *xpw_load_icon(const char *name_or_path, int px);
/* Draws such an icon, scaled to size x size, with its corner at x, y. */
void xpw_draw_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size);
/* The placeholder (a letter square of title's first letter) for a widget
 * with no icon. */
void xpw_draw_fallback_icon(XpwWidget *w, cairo_t *cr, double x, double y, double size, const char *title);

/* ---- doing things ---- */

/* Runs `cmd` through sh -c, detached (never waited on, not a child of the
 * panel). */
void xpw_run(const char *cmd);
/* Opens xisserve anchored to this widget, in the panel's colors and font,
 * with `extra_args` (e.g. "--kiremote") appended; cmd_name NULL = xisserve.
 * How every built-in widget opens its page. */
void xpw_open_xisserve(XpwWidget *w, const char *cmd_name, const char *extra_args);
/* The last completed stdout of `cmd`, run in the background at most every
 * refresh_ms and never waited for: returns a generation number, 0 until
 * the first run finishes, then +1 per completed run, with *out_text that
 * run's output (valid until the next xpw_async() on any command). */
uint64_t xpw_async(const char *cmd, unsigned refresh_ms, const char **out_text);

#endif
