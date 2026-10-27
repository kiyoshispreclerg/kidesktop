/* libkitheme - KiDesktop's cross-toolkit widget theme.
 *
 * One theme description (kitheme.conf: a preset plus overrides) and one
 * Cairo renderer, shared by every toolkit adapter:
 *   gtk2/   a real GTK2 engine (libkitheme.so) drawing with these functions
 *   export/ kitheme-export: writes the GTK2 gtkrc and GTK3/GTK4 CSS (+ PNG
 *           assets rendered by these same functions) into a theme dir
 *   qt/     a QStyle plugin (the one C++ file) that paints through these
 *           functions into a QImage
 *
 * Static library, compiled into each adapter: no shared object to install
 * and no ABI to keep. Every drawing function takes a rectangle in Cairo
 * user units and only draws inside it. */
#ifndef KITHEME_H
#define KITHEME_H

#include <cairo.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double r, g, b, a;
} KtColor;

/* Overall look of every bevelled element. */
typedef enum {
    KT_STYLE_SOFT = 0,   /* rounded, gradients, inner highlight (Oxygen-like) */
    KT_STYLE_CLASSIC,    /* square two-pixel 3D bevels (Windows 9x-like) */
    KT_STYLE_FLAT,       /* rounded, one border, no gradient */
} KtStyle;

typedef enum {
    KT_FOCUS_GLOW = 0,
    KT_FOCUS_DOTTED,
    KT_FOCUS_NONE,
} KtFocus;

typedef enum {
    KT_PTEXT_AUTO = 0, /* only when the application asks for it */
    KT_PTEXT_ALWAYS,   /* the percentage is drawn even when it does not */
} KtProgressText;

typedef struct {
    char preset[64];   /* shape preset: everything but the colors */
    char colors[64];   /* color scheme the colors came from (informational) */
    KtStyle style;

    KtColor window_bg, window_fg;
    KtColor base_bg, base_fg;         /* entries, lists, text views */
    KtColor button_bg, button_fg;
    KtColor selection_bg, selection_fg;
    KtColor accent;                   /* hover and focus glow */
    KtColor progress;                 /* progress bar fill */
    KtColor menu_bg, menu_fg;
    KtColor tooltip_bg, tooltip_fg;

    /* For kiconf's simple color editor only (no toolkit reads them): the
     * two colors kt_colors_derive() takes, and whether that editor is in
     * use (0: the 14 colors above were edited one by one). */
    KtColor simple_accent, simple_bg;
    int simple;

    double radius;     /* corner radius in px (classic ignores it) */
    double menu_radius; /* popup menu corner radius in px (0 = square; classic ignores it) */
    double relief;     /* 0..1 strength of highlights and shadows */
    double gradient;   /* 0..1 strength of the vertical gradients */
    double shadow;     /* 0..1 drop shadow under raised elements */
    KtFocus focus;
    int hover_glow;    /* accent outline on hover */

    int progress_thickness;    /* px */
    KtProgressText progress_text;
    int progress_stripes;      /* diagonal stripes on the fill */
    int progress_animation;    /* stripes move (costs a redraw every 50 ms) */

    int scrollbar_width;       /* px */
    int scrollbar_arrows;      /* stepper buttons at the ends */
    int check_size;            /* px, check boxes and radio buttons */

    /* Behavior, not looks: a left-button drag on an empty part of a menu
     * bar or toolbar moves the window (_NET_WM_MOVERESIZE). Qt and GTK2
     * only; GTK3/4 have no engine to do it from. */
    int window_drag;
} KtTheme;

/* State bits, combined. */
enum {
    KT_HOVER = 1 << 0,
    KT_PRESSED = 1 << 1,   /* also "active"/"sunken" toggles */
    KT_DISABLED = 1 << 2,
    KT_FOCUSED = 1 << 3,
    KT_DEFAULT = 1 << 4,   /* the dialog's default button */
    KT_CHECKED = 1 << 5,
    KT_MIXED = 1 << 6,     /* inconsistent check box */
    KT_SELECTED = 1 << 7,  /* tabs, menu items, list rows */
    KT_NOBOX = 1 << 8,     /* check/radio: the mark alone, in base_fg (menus) */
};

typedef enum { KT_UP, KT_DOWN, KT_LEFT, KT_RIGHT } KtDir;

#define KITHEME_VERSION "0.1.4"

/* ---- theme loading (kt_config.c) ---- */

/* The built-in defaults (the "oxygen" preset's values), no files read. */
void kt_theme_defaults(KtTheme *t);
/* Loads `path` (a kitheme.conf): its `preset =` file first, then its
 * `colors =` scheme, then its own keys on top. Returns 0 when the file could not be read (t still holds
 * the defaults). */
int kt_theme_load_file(KtTheme *t, const char *path);
/* Loads preset `name` (shape and sizes; presets carry no colors) from the
 * presets directories. */
int kt_theme_load_preset(KtTheme *t, const char *name);
/* Applies color scheme `name` (colors/NAME.conf in the same search order
 * as presets) on top of `t`, leaving the shape alone. Returns 0 when it is
 * not found. */
int kt_theme_load_colors(KtTheme *t, const char *name);
/* Writes the 14 colors of `t` (and its simple_* keys) as user scheme `name` in
 * ~/.local/share/kitheme/colors. Returns 0 on failure. */
int kt_theme_save_colors(const KtTheme *t, const char *name);
/* Removes user scheme `name` (the installed ones cannot be removed). */
int kt_colors_delete(const char *name);
/* True when `name` exists in the user's own directory. */
int kt_colors_is_user(const char *name);
/* Same as kt_list_presets() for the color schemes. */
int kt_list_colorschemes(char names[][64], int max);
/* The user's theme: $XDG_CONFIG_HOME/kitheme/kitheme.conf, or the default
 * preset when there is none. */
void kt_theme_load_user(KtTheme *t);
/* Path of the user's kitheme.conf (whether it exists or not). */
void kt_user_conf_path(char *out, size_t sz);
/* Writes every key of `t` to `path` (atomically). Returns 0 on failure. */
int kt_theme_save(const KtTheme *t, const char *path);
/* Fills `names` with up to `max` preset names found on disk, sorted;
 * returns how many. */
int kt_list_presets(char names[][64], int max);

/* ---- colors (kt_color.c) ---- */

int kt_color_parse(const char *s, KtColor *out);
/* "#rrggbb" (alpha ignored). */
void kt_color_hex(KtColor c, char out[8]);
/* "rgba(r,g,b,a)" for CSS. */
void kt_color_css(KtColor c, char *out, size_t sz);
/* Multiplies lightness by k (HLS), like GTK's own shading. */
KtColor kt_shade(KtColor c, double k);
/* t=0 gives a, t=1 gives b. */
KtColor kt_mix(KtColor a, KtColor b, double t);
KtColor kt_alpha(KtColor c, double a);
/* Perceived lightness, 0..1. */
double kt_luma(KtColor c);
/* Sets the 14 colors of `t` from two: `accent` gives the selection, focus,
 * progress and tooltip text colors, `bg` the window background, with the
 * rest shaded from it (fields and buttons toward white on a light
 * background, toward black on a dark one; tooltips on the opposite side).
 * Also stores both in simple_accent/simple_bg. */
void kt_colors_derive(KtTheme *t, KtColor accent, KtColor bg);

/* ---- drawing (kt_draw.c) ---- */

void kt_draw_button(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st, KtColor bg);
/* Sunken field (entry, spin box, list frame); `bg` is its base color. */
void kt_draw_entry(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st, KtColor bg);
/* Plain frame: sunken > 0 inset, < 0 raised, 0 etched. */
void kt_draw_frame(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int sunken, KtColor bg);
void kt_draw_check(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st);
void kt_draw_radio(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st);
void kt_draw_arrow(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, KtDir dir, KtColor fg);
void kt_draw_progress_trough(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical);
/* `phase` (0..1) shifts the stripes; pass 0 when not animating. */
void kt_draw_progress_bar(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical, double phase);
void kt_draw_scroll_trough(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical);
void kt_draw_scroll_slider(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical, unsigned st);
void kt_draw_scale_groove(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical, double fill_to);
void kt_draw_scale_handle(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st);
/* A tab whose open side (toward the page) is `gap`. */
void kt_draw_tab(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, KtDir gap, unsigned st);
void kt_draw_tab_pane(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h);
void kt_draw_menu(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h);
/* Effective popup menu corner radius: 0 for square menus. The toolkit
 * adapters clip the menu window to this (SHAPE mask) because a menu is a
 * rectangular X window. */
double kt_menu_radius(const KtTheme *t);
void kt_draw_menuitem(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int in_menubar);
void kt_draw_tooltip(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h);
void kt_draw_header(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st);
void kt_draw_row_selection(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int focused);
void kt_draw_separator(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical);
void kt_draw_focus(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h);

/* Every element above in one mock window (kt_sample.c): kiconf's live
 * preview and `kitheme-export --sample`. */
void kt_draw_sample(const KtTheme *t, cairo_t *cr, double w, double h);

#ifdef __cplusplus
}
#endif

#endif /* KITHEME_H */
