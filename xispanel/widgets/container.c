/*
 * container widget - a single slot on a panel that opens a second panel
 * (a PANEL line with mode=container, named by this widget's name=) as a
 * popup beside it. The popup is a real Panel with its own WIDGET/THEME
 * lines, so this widget owns nothing but the icon and the click; every
 * widget inside the popup is laid out, painted and clicked exactly as if
 * it sat on a bar -- see "Container popups" in PROTOCOL.md and
 * panel_container_toggle() in xispanel.c.
 *
 * By default it draws only the small chevron a combobox/select uses to
 * say "this opens": even with nothing else showing on the bar, the user
 * can see there's more behind it. icon=<path> replaces that with an image
 * (same loader launcher/folder use). The hover tooltip lists what's
 * inside -- "Container" plus the type name of every widget in the popup
 * -- so the arrow alone is enough to find out what it hides.
 *
 * hotkey=<spec> toggles the popup from the keyboard (same spec syntax
 * folder/xisserve use, see hotkey_register()).
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char name[64];         /* the mode=container PANEL this opens */
    cairo_surface_t *icon; /* NULL = draw the chevron */
} ContainerPriv;

static void container_toggle_hotkey(PanelWidget *w)
{
    Panel *popup = panel_container_popup(w);
    if (popup) {
        panel_container_toggle(popup);
    }
}

static int container_init(PanelWidget *w)
{
    ContainerPriv *cp = w->priv;
    if (!kv_get(w->config_kv, "name", cp->name, sizeof(cp->name)) || !cp->name[0]) {
        fprintf(stderr, "xispanel: container: missing required name=, widget will do nothing\n");
    }
    char icon_path[PATH_MAX];
    if (kv_get(w->config_kv, "icon", icon_path, sizeof(icon_path)) && icon_path[0]) {
        /* w->thickness isn't resolved yet this early -- see folder_init(). */
        int thickness = w->thickness > 0 ? w->thickness : w->panel->thickness_cfg;
        int icon_px = thickness > 6 ? thickness - 6 : 16;
        cp->icon = load_icon_argb(icon_path, icon_fetch_size_for(icon_px));
        if (!cp->icon) {
            fprintf(stderr, "xispanel: container: could not load icon '%s', drawing the arrow instead\n", icon_path);
        }
    }
    char hotkey[64];
    if (kv_get(w->config_kv, "hotkey", hotkey, sizeof(hotkey)) && hotkey[0]) {
        hotkey_register(w, hotkey, container_toggle_hotkey);
    }
    return 0;
}

static void container_destroy(PanelWidget *w)
{
    ContainerPriv *cp = w->priv;
    if (cp->icon) {
        cairo_surface_destroy(cp->icon);
    }
    hotkey_unregister_widget(w);
}

static void container_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    ContainerPriv *cp = w->priv;
    /* The bare chevron doesn't need a full square slot the way an icon
     * does -- a bit over half the thickness keeps it from wasting bar
     * space while staying an easy click target. */
    *out_len = cp->icon ? cross_axis : (cross_axis * 5) / 8;
    *out_min_len = *out_len;
}

/* The combobox chevron: a stroked "v" pointing away from the bar, i.e.
 * toward where the popup will appear -- down on a top panel, up on a
 * bottom one, sideways on left/right. Flips to point back at the bar
 * while the popup is open, the same way an expanded select's arrow does. */
static void draw_chevron(cairo_t *cr, double cx, double cy, double size, enum edge edge, int open, double r,
                         double g, double b, double a)
{
    double half = size / 2.0;
    double depth = size * 0.28;
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, size > 12 ? 2.0 : 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    int away = open ? -1 : 1;
    switch (edge) {
    case EDGE_TOP:
        cairo_move_to(cr, cx - half, cy - depth * away / 2.0);
        cairo_line_to(cr, cx, cy + depth * away / 2.0);
        cairo_line_to(cr, cx + half, cy - depth * away / 2.0);
        break;
    case EDGE_BOTTOM:
        cairo_move_to(cr, cx - half, cy + depth * away / 2.0);
        cairo_line_to(cr, cx, cy - depth * away / 2.0);
        cairo_line_to(cr, cx + half, cy + depth * away / 2.0);
        break;
    case EDGE_LEFT:
        cairo_move_to(cr, cx - depth * away / 2.0, cy - half);
        cairo_line_to(cr, cx + depth * away / 2.0, cy);
        cairo_line_to(cr, cx - depth * away / 2.0, cy + half);
        break;
    case EDGE_RIGHT:
        cairo_move_to(cr, cx + depth * away / 2.0, cy - half);
        cairo_line_to(cr, cx - depth * away / 2.0, cy);
        cairo_line_to(cr, cx + depth * away / 2.0, cy + half);
        break;
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void container_paint(PanelWidget *w, cairo_t *cr)
{
    ContainerPriv *cp = w->priv;
    Panel *p = w->panel;
    Panel *popup = panel_container_popup(w);
    int open = popup && popup->open;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);

    /* Keep the slot highlighted the whole time the popup is up, not just
     * while hovered -- it's the one bar element "in use" right now. */
    if (open) {
        widget_paint_hover_rect(w, cr, 0, w->len);
    } else {
        widget_paint_hover_bg(w, cr);
    }

    if (cp->icon) {
        int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;
        draw_icon_scaled(cr, cp->icon, ox + (owidth - icon_px) / 2.0, oy + (oheight - icon_px) / 2.0, icon_px);
        return;
    }
    cairo_surface_t *themed = panel_theme_icon(p, open ? "container-open" : "container", w->thickness);
    if (themed) {
        int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;
        draw_icon_scaled(cr, themed, ox + (owidth - icon_px) / 2.0, oy + (oheight - icon_px) / 2.0, icon_px);
        return;
    }
    /* The chevron is drawn in the widget's *content* frame, which
     * panel_paint_content() rotates by p->rotate -- so the screen-space
     * "away from the bar" direction has to be un-rotated into that frame
     * first, then read back as the edge whose chevron points that way. */
    int ax = (p->edge == EDGE_LEFT) ? 1 : (p->edge == EDGE_RIGHT) ? -1 : 0;
    int ay = (p->edge == EDGE_TOP) ? 1 : (p->edge == EDGE_BOTTOM) ? -1 : 0;
    for (int r = 0; r < p->rotate; r += 90) {
        int nx = ay, ny = -ax; /* one quarter turn back */
        ax = nx;
        ay = ny;
    }
    enum edge e = (ay > 0) ? EDGE_TOP : (ay < 0) ? EDGE_BOTTOM : (ax > 0) ? EDGE_LEFT : EDGE_RIGHT;
    double size = w->thickness * 0.34;
    draw_chevron(cr, ox + owidth / 2.0, oy + oheight / 2.0, size, e, open, p->fg_r, p->fg_g, p->fg_b, p->fg_a);
}

static int container_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                                  int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    Panel *popup = panel_container_popup(w);
    if (!popup || popup->open) {
        return 0; /* nothing to open, or it's already showing itself */
    }
    size_t used = (size_t)snprintf(buf, bufsz, "Container");
    for (int i = 0; i < popup->n_widgets && used < bufsz; i++) {
        used += (size_t)snprintf(buf + used, bufsz - used, "%s%s", i == 0 ? "\n" : ", ",
                                 popup->widgets[i].ops->type_name);
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static int container_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    if (button != Button1) {
        return 0;
    }
    Panel *popup = panel_container_popup(w);
    if (!popup) {
        return 0;
    }
    panel_container_toggle(popup);
    return 1;
}

const PanelWidgetOps container_ops = {
    .type_name = "container",
    .priv_size = sizeof(ContainerPriv),
    .init = container_init,
    .destroy = container_destroy,
    .measure = container_measure,
    .paint = container_paint,
    .on_button = container_on_button,
    .get_tooltip = container_get_tooltip,
};
