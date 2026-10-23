/* kp_palette.c - the color bar at the bottom, like Paint's.
 *
 * Left: color 1 over color 2 (click one to edit it). Right: 2 rows of
 * 14 colors; left click picks color 1, right click color 2, a double
 * click edits that palette entry. */
#include "kp.h"

#define CELL 16
#define GAP 2
#define COLS (KP_PALETTE_SIZE / 2)
#define CUR 36              /* the color 1/2 box */
#define GRID_X (CUR + 10)
#define SWATCH 22           /* each square in the color 1/2 box */

static GtkWidget *g_area;

static void fill_swatch(cairo_t *cr, double x, double y, double s, const KpColor *c)
{
    cairo_rectangle(cr, x + 0.5, y + 0.5, s - 1, s - 1);
    cairo_set_source_rgb(cr, c->r, c->g, c->b);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)data;
    cairo_t *cr = gdk_cairo_create(w->window);
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);
    fill_swatch(cr, CUR - SWATCH, CUR - SWATCH, SWATCH, &kp.color[1]);
    fill_swatch(cr, 0, 0, SWATCH, &kp.color[0]);
    for (int i = 0; i < KP_PALETTE_SIZE; i++)
        fill_swatch(cr, GRID_X + (i % COLS) * (CELL + GAP), (i / COLS) * (CELL + GAP), CELL, &kp.palette[i]);
    cairo_destroy(cr);
    return TRUE;
}

/* Palette index under (x,y), or -1. */
static int cell_at(double x, double y)
{
    x -= GRID_X;
    if (x < 0 || y < 0)
        return -1;
    int c = (int)x / (CELL + GAP), r = (int)y / (CELL + GAP);
    if (c >= COLS || r >= 2 || (int)x % (CELL + GAP) >= CELL || (int)y % (CELL + GAP) >= CELL)
        return -1;
    return r * COLS + c;
}

static gboolean edit_color(const char *title, KpColor *c)
{
    GtkWidget *dlg = gtk_color_selection_dialog_new(title);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(kp.win));
    GtkColorSelection *sel = GTK_COLOR_SELECTION(GTK_COLOR_SELECTION_DIALOG(dlg)->colorsel);
    GdkColor gc = { 0, (guint16)(c->r * 65535), (guint16)(c->g * 65535), (guint16)(c->b * 65535) };
    gtk_color_selection_set_current_color(sel, &gc);
    gtk_color_selection_set_has_opacity_control(sel, FALSE);
    gboolean ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
    if (ok) {
        gtk_color_selection_get_current_color(sel, &gc);
        c->r = gc.red / 65535.0;
        c->g = gc.green / 65535.0;
        c->b = gc.blue / 65535.0;
        c->a = 1;
    }
    gtk_widget_destroy(dlg);
    return ok;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->button != 1 && ev->button != 3)
        return FALSE;
    int slot = ev->button == 3;
    int i = cell_at(ev->x, ev->y);
    if (i >= 0) {
        if (ev->type == GDK_2BUTTON_PRESS) {
            if (edit_color(_("Edit color"), &kp.palette[i])) {
                kp.color[slot] = kp.palette[i];
                kp_config_save();
            }
        } else if (ev->type == GDK_BUTTON_PRESS) {
            kp.color[slot] = kp.palette[i];
        }
        kp_palette_refresh();
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS || ev->x >= CUR || ev->y >= CUR)
        return FALSE;
    /* color 1 is drawn on top: its square wins where they overlap */
    gboolean first = ev->x < SWATCH && ev->y < SWATCH;
    if (edit_color(first ? _("Color 1") : _("Color 2"), &kp.color[first ? 0 : 1]))
        kp_palette_refresh();
    return TRUE;
}

static gboolean on_query_tooltip(GtkWidget *w, int x, int y, gboolean kb, GtkTooltip *tip, gpointer data)
{
    (void)w;
    (void)kb;
    (void)data;
    if (cell_at(x, y) >= 0) {
        gtk_tooltip_set_text(tip, _("Left click: color 1. Right click: color 2. Double click: edit."));
        return TRUE;
    }
    if (x < CUR && y < CUR) {
        gtk_tooltip_set_text(tip, _("Color 1 (left button) and color 2 (right button). Click to edit, X swaps them."));
        return TRUE;
    }
    return FALSE;
}

void kp_palette_refresh(void)
{
    if (g_area)
        gtk_widget_queue_draw(g_area);
}

GtkWidget *kp_palette_new(void)
{
    g_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_area, GRID_X + COLS * (CELL + GAP), CUR);
    gtk_widget_add_events(g_area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(g_area, "expose-event", G_CALLBACK(on_expose), NULL);
    g_signal_connect(g_area, "button-press-event", G_CALLBACK(on_press), NULL);
    gtk_widget_set_has_tooltip(g_area, TRUE);
    g_signal_connect(g_area, "query-tooltip", G_CALLBACK(on_query_tooltip), NULL);

    GtkWidget *box = gtk_hbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    gtk_box_pack_start(GTK_BOX(box), g_area, FALSE, FALSE, 4);
    return box;
}
