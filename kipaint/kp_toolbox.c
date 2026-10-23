/* kp_toolbox.c - the tool buttons (2 columns, Paint style) and, under
 * them, the options of the current tool. Icons are drawn by each tool, so
 * they don't depend on the icon theme. */
#include "kp.h"

const KpTool *const kp_tools[] = {
    &kp_tool_pencil, &kp_tool_brush, &kp_tool_eraser, &kp_tool_fill, &kp_tool_picker,
};
const int kp_n_tools = G_N_ELEMENTS(kp_tools);

static GtkWidget *g_buttons[G_N_ELEMENTS(kp_tools)];
static GtkWidget *g_size_box, *g_size_spin, *g_transparent;
static gboolean g_syncing;

int kp_tool_index(const KpTool *t)
{
    for (int i = 0; i < kp_n_tools; i++)
        if (kp_tools[i] == t)
            return i;
    return 0;
}

int kp_tool_size(void)
{
    return kp.sizes[kp_tool_index(kp.tool)];
}

static void options_sync(void)
{
    g_syncing = TRUE;
    if (kp.tool->max_size) {
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(g_size_spin), 1, kp.tool->max_size);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_size_spin), kp_tool_size());
        gtk_widget_show(g_size_box);
    } else {
        gtk_widget_hide(g_size_box);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_transparent), kp.eraser_transparent);
    if (kp.tool == &kp_tool_eraser)
        gtk_widget_show(g_transparent);
    else
        gtk_widget_hide(g_transparent);
    g_syncing = FALSE;
}

/* [ and ] */
void kp_tool_size_step(int dir)
{
    const KpTool *t = kp.tool;
    if (!t->max_size)
        return;
    int *s = &kp.sizes[kp_tool_index(t)];
    int step = *s < 10 ? 1 : *s < 50 ? 5 : 10;
    *s = CLAMP(*s + dir * step, 1, t->max_size);
    options_sync();
    kp_canvas_tool_changed();
}

void kp_toolbox_select(const KpTool *t)
{
    if (t != kp.tool) {
        /* the picker hands back to whatever was in use before it */
        if (t == &kp_tool_picker)
            kp.prev_tool = kp.tool;
        kp.tool = t;
    }
    g_syncing = TRUE;
    for (int i = 0; i < kp_n_tools; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_buttons[i]), kp_tools[i] == t);
    g_syncing = FALSE;
    options_sync();
    kp_canvas_tool_changed();
}

static void on_toggled(GtkToggleButton *b, gpointer data)
{
    if (g_syncing)
        return;
    const KpTool *t = data;
    if (!gtk_toggle_button_get_active(b) && t == kp.tool) {
        /* clicking the current tool keeps it selected */
        g_syncing = TRUE;
        gtk_toggle_button_set_active(b, TRUE);
        g_syncing = FALSE;
        return;
    }
    if (gtk_toggle_button_get_active(b))
        kp_toolbox_select(t);
    kp_canvas_grab_focus();
}

static GtkWidget *tool_icon(const KpTool *t)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
    cairo_t *cr = cairo_create(s);
    t->draw_icon(cr);
    cairo_destroy(cr);
    GdkPixbuf *pb = kp_surface_to_pixbuf(s, FALSE);
    cairo_surface_destroy(s);
    GtkWidget *img = gtk_image_new_from_pixbuf(pb);
    g_object_unref(pb);
    return img;
}

static void on_size_changed(GtkSpinButton *s, gpointer data)
{
    (void)data;
    if (g_syncing)
        return;
    kp.sizes[kp_tool_index(kp.tool)] = gtk_spin_button_get_value_as_int(s);
    kp_canvas_tool_changed();
}

static void on_transparent(GtkToggleButton *b, gpointer data)
{
    (void)data;
    if (!g_syncing)
        kp.eraser_transparent = gtk_toggle_button_get_active(b);
}

GtkWidget *kp_toolbox_new(void)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 4);

    int rows = (kp_n_tools + 1) / 2;
    GtkWidget *table = gtk_table_new(rows, 2, TRUE);
    for (int i = 0; i < kp_n_tools; i++) {
        const KpTool *t = kp_tools[i];
        GtkWidget *b = gtk_toggle_button_new();
        gtk_button_set_focus_on_click(GTK_BUTTON(b), FALSE);
        gtk_container_add(GTK_CONTAINER(b), tool_icon(t));
        char *tip = g_strdup_printf("%s (%s)", _(t->name), gdk_keyval_name(gdk_keyval_to_upper(t->key)));
        gtk_widget_set_tooltip_text(b, tip);
        g_free(tip);
        g_signal_connect(b, "toggled", G_CALLBACK(on_toggled), (gpointer)t);
        gtk_table_attach(GTK_TABLE(table), b, i % 2, i % 2 + 1, i / 2, i / 2 + 1, GTK_FILL, GTK_FILL, 0, 0);
        g_buttons[i] = b;
    }
    gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, FALSE, 0);

    g_size_box = gtk_vbox_new(FALSE, 2);
    GtkWidget *label = gtk_label_new(_("Size"));
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(g_size_box), label, FALSE, FALSE, 0);
    g_size_spin = gtk_spin_button_new_with_range(1, 200, 1);
    gtk_widget_set_tooltip_text(g_size_spin, _("Size in pixels ([ and ] change it)"));
    g_signal_connect(g_size_spin, "value-changed", G_CALLBACK(on_size_changed), NULL);
    gtk_box_pack_start(GTK_BOX(g_size_box), g_size_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), g_size_box, FALSE, FALSE, 0);

    g_transparent = gtk_check_button_new_with_mnemonic(_("_Transparent"));
    gtk_widget_set_tooltip_text(g_transparent, _("Erase to transparency instead of painting color 2"));
    g_signal_connect(g_transparent, "toggled", G_CALLBACK(on_transparent), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_transparent, FALSE, FALSE, 0);

    gtk_widget_show_all(vbox);
    /* As wide as with every option shown, so switching tools never moves
     * the canvas. */
    GtkRequisition req;
    gtk_widget_size_request(vbox, &req);
    gtk_widget_set_size_request(vbox, req.width, -1);
    gtk_widget_set_no_show_all(g_size_box, TRUE);
    gtk_widget_set_no_show_all(g_transparent, TRUE);
    return vbox;
}
