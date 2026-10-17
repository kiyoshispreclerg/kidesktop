/*
 * modules.c - installable widget modules (<type>.so, see
 * xispanel_widget.h) behind the same PanelWidgetOps every built-in widget
 * uses, so panel_add_widget() and everything past it treat them alike.
 *
 * The built-in widgets stay compiled in, with full access to the panel's
 * internals; a module only ever sees the small versioned interface in the
 * public header, which this file implements on top of those internals.
 * Nothing is loaded until a panel uses the type: startup only lists the
 * directories (names, no dlopen), so an installed module no panel uses
 * costs a directory entry.
 *
 * Wiring: each module gets an ExtOps whose first member is the
 * PanelWidgetOps handed to the panel, so a wrapper below recovers the
 * module from w->ops alone -- PanelWidgetOps functions are all a widget
 * instance ever gets called through.
 */
#include "xispanel.h"
#include "xispanel_widget.h"

#include <dlfcn.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XISPANEL_WIDGETDIR
#define XISPANEL_WIDGETDIR "/usr/local/lib/xispanel/widgets"
#endif

#define MAX_WIDGET_MODULES 64

typedef struct {
    PanelWidgetOps ops; /* first: w->ops points here */
    const XispanelWidgetModule *mod;
    char *type;
    char *path;
    int tried; /* loaded, or failed once -- never retried, so a broken .so logs once per run */
    int usable;
} ExtOps;

static ExtOps g_ext[MAX_WIDGET_MODULES];
static int g_n_ext;

static const XispanelWidgetModule *ext_mod(const PanelWidget *w)
{
    return ((const ExtOps *)w->ops)->mod;
}

/* ---- the interface the modules see (exported via widgets-api.sym) ---- */

static PanelWidget *pw(XpwWidget *w) { return (PanelWidget *)w; }

void *xpw_priv(XpwWidget *w) { return pw(w)->priv; }

int xpw_option(XpwWidget *w, const char *key, char *out, size_t outsz)
{
    return kv_get(pw(w)->config_kv, key, out, outsz);
}

int xpw_option_int(XpwWidget *w, const char *key, int defval)
{
    return kv_get_int(pw(w)->config_kv, key, defval);
}

void xpw_geometry(XpwWidget *w, int *x, int *y, int *width, int *height)
{
    int lx, ly, lw, lh;
    widget_get_rect(pw(w), &lx, &ly, &lw, &lh);
    if (x) *x = lx;
    if (y) *y = ly;
    if (width) *width = lw;
    if (height) *height = lh;
}

int xpw_is_vertical(XpwWidget *w)
{
    return pw(w)->panel->edge == EDGE_LEFT || pw(w)->panel->edge == EDGE_RIGHT;
}

void xpw_colors(XpwWidget *w, double fg[4], double bg[4])
{
    const Panel *p = pw(w)->panel;
    if (fg) {
        fg[0] = p->fg_r, fg[1] = p->fg_g, fg[2] = p->fg_b, fg[3] = p->fg_a;
    }
    if (bg) {
        bg[0] = p->bg_r, bg[1] = p->bg_g, bg[2] = p->bg_b, bg[3] = p->bg_a;
    }
}

void xpw_tick_at(XpwWidget *w, uint64_t at_ms) { pw(w)->next_tick_ms = at_ms; }

void xpw_redraw(XpwWidget *w) { pw(w)->panel->dirty = 1; }

uint64_t xpw_now_ms(void) { return now_ms(); }

void xpw_paint_hover_bg(XpwWidget *w, cairo_t *cr) { widget_paint_hover_bg(pw(w), cr); }

double xpw_text_size(XpwWidget *w) { return panel_text_size(pw(w)->panel); }

double xpw_text_width(XpwWidget *w, const char *text)
{
    Panel *p = pw(w)->panel;
    double tw = 0;
    pango_text_extents_panel(p->cr, text ? text : "", panel_text_size(p), 0, &tw, NULL, p);
    return tw;
}

void xpw_draw_text(XpwWidget *w, cairo_t *cr, double x, double top_y, double box_h, const char *text)
{
    Panel *p = pw(w)->panel;
    pango_show_text_boxed(cr, x, top_y, box_h, 0, panel_text_size(p), text ? text : "", NULL, p);
}

cairo_surface_t *xpw_load_icon(const char *name_or_path, int px)
{
    if (!name_or_path || !*name_or_path || px <= 0) {
        return NULL;
    }
    return resolve_icon_theme_name(name_or_path, icon_fetch_size_for(px));
}

void xpw_draw_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size)
{
    draw_icon_scaled(cr, icon, x, y, size);
}

void xpw_draw_fallback_icon(XpwWidget *w, cairo_t *cr, double x, double y, double size, const char *title)
{
    Panel *p = pw(w)->panel;
    draw_fallback_icon(cr, x, y, size, title ? title : "", p->fg_r, p->fg_g, p->fg_b, panel_text_size(p));
}

void xpw_run(const char *cmd) { run_detached(cmd); }

void xpw_open_xisserve(XpwWidget *w, const char *cmd_name, const char *extra_args)
{
    xisserve_spawn_for_widget(pw(w), cmd_name, extra_args);
}

uint64_t xpw_async(const char *cmd, unsigned refresh_ms, const char **out_text)
{
    return asyncmd_get(cmd, refresh_ms, out_text);
}

/* ---- PanelWidgetOps over a module ---- */

static int ext_init(PanelWidget *w)
{
    const XispanelWidgetModule *m = ext_mod(w);
    return m->init ? m->init((XpwWidget *)w) : 0;
}

static void ext_destroy(PanelWidget *w)
{
    const XispanelWidgetModule *m = ext_mod(w);
    if (m->destroy) {
        m->destroy((XpwWidget *)w);
    }
}

static void ext_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    const XispanelWidgetModule *m = ext_mod(w);
    *out_len = *out_min_len = cross_axis;
    if (m->measure) {
        m->measure((XpwWidget *)w, cross_axis, out_len, out_min_len);
    }
}

static void ext_paint(PanelWidget *w, cairo_t *cr)
{
    const XispanelWidgetModule *m = ext_mod(w);
    if (m->paint) {
        m->paint((XpwWidget *)w, cr);
    }
}

static int ext_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)root_x;
    (void)root_y;
    return ext_mod(w)->on_button((XpwWidget *)w, button, local_x, local_y);
}

static int ext_on_tick(PanelWidget *w, uint64_t now)
{
    return ext_mod(w)->on_tick((XpwWidget *)w, now);
}

static int ext_is_urgent(PanelWidget *w)
{
    return ext_mod(w)->is_urgent((XpwWidget *)w);
}

static int ext_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                           int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    if (!ext_mod(w)->get_tooltip((XpwWidget *)w, buf, bufsz) || !buf[0]) {
        return 0;
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

/* dlopen()s one listed module and fills in its ExtOps; a module that
 * fails is reported once and stays unusable. */
static void ext_load(ExtOps *e)
{
    e->tried = 1;
    void *h = dlopen(e->path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "xispanel: widget module %s not loaded: %s\n", e->path, dlerror());
        return;
    }
    XispanelWidgetEntryFn entry = (XispanelWidgetEntryFn)dlsym(h, XISPANEL_WIDGET_ENTRY);
    const XispanelWidgetModule *m = entry ? entry() : NULL;
    if (!m || m->abi != XISPANEL_WIDGET_ABI) {
        fprintf(stderr, "xispanel: widget module %s has an incompatible interface (expected ABI %d)\n", e->path,
                XISPANEL_WIDGET_ABI);
        dlclose(h);
        return;
    }
    e->mod = m;
    e->ops.type_name = e->type;
    e->ops.priv_size = m->priv_size;
    e->ops.embeddable = m->embeddable;
    e->ops.init = ext_init;
    e->ops.destroy = ext_destroy;
    e->ops.measure = ext_measure;
    e->ops.paint = ext_paint;
    if (m->on_button) e->ops.on_button = ext_on_button;
    if (m->on_tick) e->ops.on_tick = ext_on_tick;
    if (m->is_urgent) e->ops.is_urgent = ext_is_urgent;
    if (m->get_tooltip) e->ops.get_tooltip = ext_get_tooltip;
    e->usable = 1;
}

/* A module's type is its file name, so it has to be something a WIDGET
 * line can carry and not shadow a built-in one (checked by the caller). */
static int type_name_ok(const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || n > 31) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '-' || t[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

/* Lists `<type>.so` in each module directory -- without opening any --
 * and appends the types not seen yet. Directories in priority order:
 * $XISPANEL_WIDGET_PATH (colon-separated, for testing from a build
 * tree), ~/.local/lib/xispanel/widgets, XISPANEL_WIDGETDIR; the first to
 * provide a type wins. */
static void scan_modules(void)
{
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    const char *env = g_getenv("XISPANEL_WIDGET_PATH");
    if (env && *env) {
        char **parts = g_strsplit(env, ":", -1);
        for (char **d = parts; *d; d++) {
            if (**d) {
                g_ptr_array_add(dirs, g_strdup(*d));
            }
        }
        g_strfreev(parts);
    }
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".local", "lib", "xispanel", "widgets", NULL));
    g_ptr_array_add(dirs, g_strdup(XISPANEL_WIDGETDIR));

    for (guint d = 0; d < dirs->len; d++) {
        const char *dir = g_ptr_array_index(dirs, d);
        GDir *gd = g_dir_open(dir, 0, NULL);
        if (!gd) {
            continue;
        }
        const char *name;
        while ((name = g_dir_read_name(gd)) && g_n_ext < MAX_WIDGET_MODULES) {
            if (!g_str_has_suffix(name, ".so")) {
                continue;
            }
            char *type = g_strndup(name, strlen(name) - 3);
            int dup = !type_name_ok(type);
            for (int i = 0; !dup && i < g_n_ext; i++) {
                dup = strcmp(g_ext[i].type, type) == 0;
            }
            if (dup) {
                g_free(type);
                continue;
            }
            ExtOps *e = &g_ext[g_n_ext++];
            memset(e, 0, sizeof(*e));
            e->type = type;
            e->path = g_build_filename(dir, name, NULL);
        }
        g_dir_close(gd);
    }
    g_ptr_array_free(dirs, TRUE);
}

static ExtOps *ext_find(const char *type)
{
    for (int i = 0; i < g_n_ext; i++) {
        if (strcmp(g_ext[i].type, type) == 0) {
            return &g_ext[i];
        }
    }
    return NULL;
}

/* The ops for a module-provided widget type, loading its .so on first
 * use; NULL if there is no such module or it can't be used. An unlisted
 * type rescans the directories once first, so a module installed while
 * xispanel runs is picked up by the next config reload. */
const PanelWidgetOps *widget_module_find(const char *type_name)
{
    static int scanned;
    if (!scanned) {
        scanned = 1;
        scan_modules();
    }
    ExtOps *e = ext_find(type_name);
    if (!e) {
        scan_modules();
        e = ext_find(type_name);
    }
    if (!e) {
        return NULL;
    }
    if (!e->tried) {
        ext_load(e);
    }
    return e->usable ? &e->ops : NULL;
}
