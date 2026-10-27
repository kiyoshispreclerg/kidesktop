/* KiTheme GTK2 engine (libkitheme.so).
 *
 * A GtkStyle subclass whose draw_* functions hand every element to
 * libkitheme. The theme's gtkrc only selects this engine per widget role
 * (`engine "kitheme" { role = menu }`) and sets metrics; the colors come
 * from the user's kitheme.conf here, in init_from_rc(), so the system-wide
 * theme directory follows the user's configuration without a generated
 * copy (kitheme-export still writes one to ~/.themes for the metrics).
 * Colors an application or a user rc file set explicitly are left alone.
 *
 * kitheme.conf is read once and checked again (one stat()) whenever GTK
 * builds a style, which is what happens after a theme change or an
 * _GTK_READ_RCFILES message -- so a change applies without restarting
 * the application. */
#include <gtk/gtk.h>
#include <gmodule.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/kitheme.h"

/* ---- the shared theme ---- */

static KtTheme g_theme;
static time_t g_theme_mtime = -1;

static void drag_enabled(void);

static void theme_refresh(void)
{
    char path[4096];
    struct stat st;
    kt_user_conf_path(path, sizeof(path));
    time_t m = stat(path, &st) == 0 ? st.st_mtime : 0;
    if (m == g_theme_mtime)
        return;
    g_theme_mtime = m;
    int had_drag = g_theme.window_drag;
    kt_theme_load_user(&g_theme);
    if (g_theme.window_drag && !had_drag)
        drag_enabled();
}

/* ---- window_drag: moving the window from empty parts of bars ----
 *
 * Every GtkMenuBar and GtkToolbar realized while the option is on gets
 * three handlers (and button-1 motion added to its window's event mask).
 * A left press that lands on the bar's own window -- not on an item or a
 * child with a window of its own -- is held back; once the pointer moves
 * past the DnD threshold the WM takes over (gtk_window_begin_move_drag,
 * i.e. _NET_WM_MOVERESIZE). Turning the option off is honored at once by
 * the handlers; turning it on registers the bars already on screen. */

static GType ki_style_type; /* defined with the style classes below */
static GtkWidget *g_drag_bar; /* bar holding back a press (weak pointer) */
static gint g_drag_x, g_drag_y;

static void drag_set(GtkWidget *bar)
{
    if (g_drag_bar)
        g_object_remove_weak_pointer(G_OBJECT(g_drag_bar), (gpointer *)&g_drag_bar);
    g_drag_bar = bar;
    if (bar)
        g_object_add_weak_pointer(G_OBJECT(bar), (gpointer *)&g_drag_bar);
}

static GtkWidget *drag_window_of(GtkWidget *bar)
{
    GtkWidget *top = gtk_widget_get_toplevel(bar);
    if (!GTK_IS_WINDOW(top) || !gtk_widget_is_toplevel(top) || GTK_IS_PLUG(top))
        return NULL;
    return top;
}

static gboolean drag_press(GtkWidget *bar, GdkEventButton *ev, gpointer data)
{
    (void)data;
    gpointer owner = NULL;
    if (!g_theme.window_drag || ev->button != 1 || (ev->state & gtk_accelerator_get_default_mod_mask()))
        return FALSE;
    gdk_window_get_user_data(ev->window, &owner);
    if (owner != bar || (GTK_IS_SEPARATOR_TOOL_ITEM(bar) && !GTK_IS_TOOLBAR(gtk_widget_get_parent(bar))))
        return FALSE;
    /* The style check covers a switch to another theme after registering. */
    if (!G_TYPE_CHECK_INSTANCE_TYPE(gtk_widget_get_style(bar), ki_style_type) || !drag_window_of(bar))
        return FALSE;
    /* Double and triple clicks are held back too: the bar would take
     * them as presses on nothing. */
    if (ev->type == GDK_BUTTON_PRESS) {
        drag_set(bar);
        g_drag_x = (gint)ev->x_root;
        g_drag_y = (gint)ev->y_root;
    }
    return TRUE;
}

static gboolean drag_motion(GtkWidget *bar, GdkEventMotion *ev, gpointer data)
{
    (void)data;
    if (bar != g_drag_bar)
        return FALSE;
    if (!g_theme.window_drag || !(ev->state & GDK_BUTTON1_MASK)) {
        drag_set(NULL);
        return FALSE;
    }
    gdk_event_request_motions(ev);
    if (!gtk_drag_check_threshold(bar, g_drag_x, g_drag_y, (gint)ev->x_root, (gint)ev->y_root))
        return TRUE;
    drag_set(NULL);
    GtkWidget *top = drag_window_of(bar);
    if (top)
        gtk_window_begin_move_drag(GTK_WINDOW(top), 1, g_drag_x, g_drag_y, ev->time);
    return TRUE;
}

static gboolean drag_release(GtkWidget *bar, GdkEventButton *ev, gpointer data)
{
    (void)ev;
    (void)data;
    if (bar != g_drag_bar)
        return FALSE;
    drag_set(NULL);
    return TRUE;
}

/* Toolbar separators too: they have a window of their own and take the
 * press, yet are as empty as the toolbar around them. */
static void drag_register(GtkWidget *w)
{
    if (!GTK_IS_MENU_BAR(w) && !GTK_IS_TOOLBAR(w) && !GTK_IS_SEPARATOR_TOOL_ITEM(w))
        return;
    if (g_object_get_data(G_OBJECT(w), "kt-drag"))
        return;
    g_object_set_data(G_OBJECT(w), "kt-drag", GINT_TO_POINTER(1));
    /* On a realized widget this also updates its windows' event mask. */
    gtk_widget_add_events(w, GDK_BUTTON1_MOTION_MASK);
    g_signal_connect(w, "button-press-event", G_CALLBACK(drag_press), NULL);
    g_signal_connect(w, "motion-notify-event", G_CALLBACK(drag_motion), NULL);
    g_signal_connect(w, "button-release-event", G_CALLBACK(drag_release), NULL);
}

static gboolean drag_realize_hook(GSignalInvocationHint *hint, guint n, const GValue *params, gpointer data)
{
    (void)hint;
    (void)data;
    if (g_theme.window_drag && n > 0) {
        GObject *o = g_value_get_object(&params[0]);
        if (GTK_IS_WIDGET(o))
            drag_register(GTK_WIDGET(o));
    }
    return TRUE;
}

static void drag_walk(GtkWidget *w, gpointer data)
{
    (void)data;
    if (gtk_widget_get_realized(w))
        drag_register(w);
    if (GTK_IS_CONTAINER(w))
        gtk_container_forall(GTK_CONTAINER(w), drag_walk, NULL);
}

static gboolean drag_walk_toplevels(gpointer data)
{
    (void)data;
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next)
        drag_walk(l->data, NULL);
    g_list_free(tops);
    return FALSE;
}

/* Out of the style rebuild that noticed the change. */
static void drag_enabled(void)
{
    static gulong hook;
    if (!hook) {
        /* The engine loads before any widget exists: GtkWidget's signals
         * are only registered once its class is. */
        gpointer klass = g_type_class_ref(GTK_TYPE_WIDGET);
        hook = g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, drag_realize_hook, NULL, NULL);
        g_type_class_unref(klass);
    }
    g_idle_add(drag_walk_toplevels, NULL);
}

/* ---- RC style: carries the role ---- */

typedef enum {
    ROLE_DEFAULT = 0,
    ROLE_BUTTON,
    ROLE_MENU,
    ROLE_MENUBAR,
    ROLE_TOOLTIP,
    ROLE_PROGRESS,
    ROLE_ENTRY,
} Role;

static const char *const role_names[] = {"default", "button", "menu", "menubar", "tooltip", "progress", "entry", NULL};

typedef struct {
    GtkRcStyle parent;
    Role role;
    gboolean has_role;
} KiRcStyle;

typedef struct {
    GtkRcStyleClass parent_class;
} KiRcStyleClass;

typedef struct {
    GtkStyle parent;
    Role role;
} KiStyle;

typedef struct {
    GtkStyleClass parent_class;
} KiStyleClass;

static GType ki_rc_style_type, ki_style_type;
static GtkRcStyleClass *rc_parent_class;
static GtkStyleClass *style_parent_class;

#define KI_RC_STYLE(o) ((KiRcStyle *)(o))
#define KI_STYLE(o) ((KiStyle *)(o))
#define IS_KI_RC_STYLE(o) G_TYPE_CHECK_INSTANCE_TYPE((o), ki_rc_style_type)

/* ---- colors ---- */

static GdkColor gdk(KtColor c)
{
    GdkColor g = {0, (guint16)(c.r * 65535), (guint16)(c.g * 65535), (guint16)(c.b * 65535)};
    return g;
}

static KtColor kt(const GdkColor *g)
{
    KtColor c = {g->red / 65535.0, g->green / 65535.0, g->blue / 65535.0, 1.0};
    return c;
}

/* Only where the rc file (or gtk_widget_modify_*) did not set it. GTK's
 * own built-in rc gives tooltips explicit colors, so for those the theme
 * always wins (`rc` NULL). */
static void put(GtkStyle *s, GtkRcStyle *rc, GtkRcFlags which, GtkStateType state, KtColor c)
{
    if (rc && (rc->color_flags[state] & which))
        return;
    GdkColor g = gdk(c);
    switch (which) {
    case GTK_RC_BG: s->bg[state] = g; break;
    case GTK_RC_FG: s->fg[state] = g; break;
    case GTK_RC_BASE: s->base[state] = g; break;
    case GTK_RC_TEXT: s->text[state] = g; break;
    default: break;
    }
}

static void put5(GtkStyle *s, GtkRcStyle *rc, GtkRcFlags which, KtColor normal, KtColor prelight, KtColor active,
                 KtColor selected, KtColor insensitive)
{
    put(s, rc, which, GTK_STATE_NORMAL, normal);
    put(s, rc, which, GTK_STATE_PRELIGHT, prelight);
    put(s, rc, which, GTK_STATE_ACTIVE, active);
    put(s, rc, which, GTK_STATE_SELECTED, selected);
    put(s, rc, which, GTK_STATE_INSENSITIVE, insensitive);
}

static void apply_colors(GtkStyle *s, GtkRcStyle *rc, Role role)
{
    if (role == ROLE_TOOLTIP)
        rc = NULL;
    const KtTheme *t = &g_theme;
    KtColor bg = t->window_bg, fg = t->window_fg;
    if (role == ROLE_BUTTON)
        bg = t->button_bg, fg = t->button_fg;
    else if (role == ROLE_MENU)
        bg = t->menu_bg, fg = t->menu_fg;
    else if (role == ROLE_TOOLTIP)
        bg = t->tooltip_bg, fg = t->tooltip_fg;
    KtColor dim = kt_mix(fg, bg, 0.55);
    KtColor sel = t->selection_bg, self = t->selection_fg;

    put5(s, rc, GTK_RC_BG, bg, kt_shade(bg, 1.04), kt_shade(bg, 0.9), sel, bg);
    put5(s, rc, GTK_RC_FG, fg, fg, fg, self, dim);
    put5(s, rc, GTK_RC_BASE, t->base_bg, t->base_bg, kt_mix(sel, t->base_bg, 0.45), sel, bg);
    put5(s, rc, GTK_RC_TEXT, t->base_fg, t->base_fg, self, self, kt_mix(t->base_fg, t->base_bg, 0.55));

    if (role == ROLE_MENU || role == ROLE_MENUBAR) {
        /* A highlighted menu item is PRELIGHT. */
        put(s, rc, GTK_RC_BG, GTK_STATE_PRELIGHT, sel);
        put(s, rc, GTK_RC_FG, GTK_STATE_PRELIGHT, self);
    } else if (role == ROLE_PROGRESS) {
        /* GtkProgressBar draws the part of its text over the bar in
         * PRELIGHT, and the bar itself in SELECTED. */
        put(s, rc, GTK_RC_FG, GTK_STATE_PRELIGHT, self);
        put(s, rc, GTK_RC_BG, GTK_STATE_SELECTED, t->progress);
    }
}

/* ---- drawing helpers ---- */

static cairo_t *begin(GdkWindow *window, GdkRectangle *area)
{
    cairo_t *cr = gdk_cairo_create(window);
    if (area) {
        gdk_cairo_rectangle(cr, area);
        cairo_clip(cr);
    }
    return cr;
}

#define SANITIZE_SIZE                                                   \
    do {                                                                \
        if (width == -1 && height == -1)                                \
            gdk_drawable_get_size(window, &width, &height);             \
        else if (width == -1)                                           \
            gdk_drawable_get_size(window, &width, NULL);                \
        else if (height == -1)                                          \
            gdk_drawable_get_size(window, NULL, &height);               \
    } while (0)

static int is(const gchar *detail, const char *d)
{
    return detail && !strcmp(detail, d);
}

static unsigned state_bits(GtkStateType state, GtkShadowType shadow, GtkWidget *w)
{
    unsigned st = 0;
    if (state == GTK_STATE_PRELIGHT)
        st |= KT_HOVER;
    if (state == GTK_STATE_INSENSITIVE)
        st |= KT_DISABLED;
    if (shadow == GTK_SHADOW_IN || state == GTK_STATE_ACTIVE)
        st |= KT_PRESSED;
    if (w && gtk_widget_has_focus(w))
        st |= KT_FOCUSED;
    if (w && GTK_IS_BUTTON(w) && gtk_widget_has_default(w))
        st |= KT_DEFAULT;
    return st;
}

static KtTheme style_theme(GtkStyle *style)
{
    /* The engine's theme, with this style's own colors where the widget's
     * role or an application override changed them. */
    KtTheme t = g_theme;
    t.base_bg = kt(&style->base[GTK_STATE_NORMAL]);
    t.base_fg = kt(&style->text[GTK_STATE_NORMAL]);
    return t;
}

/* ---- progress bars: stripes animation and the text inside ---- */

static GSList *g_animated;
static guint g_anim_timer;

static gboolean anim_tick(gpointer data)
{
    (void)data;
    gint64 now = g_get_monotonic_time();
    for (GSList *l = g_animated; l;) {
        GtkWidget *w = l->data;
        l = l->next;
        gint64 seen = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "kitheme-seen"));
        /* Stop animating bars that have not been drawn for a second
         * (hidden, unmapped, finished): no redraws nobody sees. */
        if (now / 1000 - seen > 1000 || !gtk_widget_is_drawable(w)) {
            g_animated = g_slist_remove(g_animated, w);
            g_object_set_data(G_OBJECT(w), "kitheme-anim", NULL);
            g_object_unref(w);
            continue;
        }
        gtk_widget_queue_draw(w);
    }
    if (!g_animated) {
        g_anim_timer = 0;
        return FALSE;
    }
    return TRUE;
}

static double anim_phase(GtkWidget *w)
{
    if (!g_theme.progress_animation || !g_theme.progress_stripes || !w)
        return 0;
    gint64 now = g_get_monotonic_time();
    g_object_set_data(G_OBJECT(w), "kitheme-seen", GSIZE_TO_POINTER((gsize)(now / 1000)));
    if (!g_object_get_data(G_OBJECT(w), "kitheme-anim")) {
        g_object_set_data(G_OBJECT(w), "kitheme-anim", GINT_TO_POINTER(1));
        g_animated = g_slist_prepend(g_animated, g_object_ref(w));
        if (!g_anim_timer)
            g_anim_timer = g_timeout_add(50, anim_tick, NULL);
    }
    return fmod(now / 1e6, 1.0);
}

/* The percentage, when the theme wants it and the application shows no
 * text of its own. Over the bar in selection_fg, elsewhere in window_fg. */
static void progress_text(GtkStyle *style, cairo_t *cr, GtkWidget *w, int bx, int by, int bw, int bh)
{
    if (g_theme.progress_text != KT_PTEXT_ALWAYS || !w || !GTK_IS_PROGRESS_BAR(w))
        return;
    GtkProgressBar *pb = GTK_PROGRESS_BAR(w);
    if (gtk_progress_bar_get_text(pb) || GTK_PROGRESS(w)->activity_mode)
        return;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int x0 = gtk_widget_get_has_window(w) ? 0 : a.x, y0 = gtk_widget_get_has_window(w) ? 0 : a.y;
    if (a.height < 12 && a.width < 12)
        return;
    char txt[16];
    g_snprintf(txt, sizeof(txt), "%d%%", (int)(gtk_progress_bar_get_fraction(pb) * 100 + 0.5));
    PangoLayout *layout = gtk_widget_create_pango_layout(w, txt);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);
    double tx = x0 + (a.width - tw) / 2, ty = y0 + (a.height - th) / 2;
    KtColor out = kt(&style->fg[GTK_STATE_NORMAL]), in = kt(&style->fg[GTK_STATE_PRELIGHT]);

    cairo_save(cr);
    cairo_rectangle(cr, x0, y0, a.width, a.height);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, out.r, out.g, out.b);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    cairo_save(cr);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, in.r, in.g, in.b);
    cairo_move_to(cr, tx, ty);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);
    g_object_unref(layout);
}

/* ---- GtkStyle vfuncs ---- */

static int in_tree_header(GtkWidget *w)
{
    GtkWidget *p = w ? gtk_widget_get_parent(w) : NULL;
    return p && GTK_IS_TREE_VIEW(p);
}

/* A menu is a rectangular X window: clip its toplevel to the rounded
 * shape kt_draw_menu() draws. Redone only when the size changes. */
static void shape_menu(GtkWidget *widget)
{
    double r = kt_menu_radius(&g_theme);
    if (r <= 0 || !widget || !GTK_IS_MENU(widget))
        return;
    GtkWidget *top = gtk_widget_get_toplevel(widget);
    if (!top || !gtk_widget_get_realized(top))
        return;
    GdkWindow *win = top->window;
    int w = top->allocation.width, h = top->allocation.height;
    if (w < 2 * r || h < 2 * r)
        return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "kt-menu-size")) == (w << 16 | h))
        return;
    g_object_set_data(G_OBJECT(win), "kt-menu-size", GINT_TO_POINTER(w << 16 | h));
    GdkBitmap *mask = gdk_pixmap_new(NULL, w, h, 1);
    cairo_t *cr = gdk_cairo_create(mask);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
    cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_destroy(cr);
    gdk_window_shape_combine_mask(win, mask, 0, 0);
    g_object_unref(mask);
}

static void ki_draw_box(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                        GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                        gint x, gint y, gint width, gint height)
{
    SANITIZE_SIZE;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    unsigned st = state_bits(state, shadow, widget);

    if (is(detail, "buttondefault")) {
        /* Folded into the button itself (KT_DEFAULT). */
    } else if (is(detail, "button") && in_tree_header(widget)) {
        kt_draw_header(&t, cr, x, y, width, height, st & ~KT_FOCUSED);
    } else if (is(detail, "button") || is(detail, "togglebutton") || is(detail, "optionmenu")) {
        if (state == GTK_STATE_INSENSITIVE && shadow == GTK_SHADOW_NONE)
            ;
        else if (shadow == GTK_SHADOW_NONE && !(st & (KT_HOVER | KT_PRESSED)))
            ; /* relief-less button (toolbars) at rest: nothing */
        else
            kt_draw_button(&t, cr, x, y, width, height, st, kt(&style->bg[GTK_STATE_NORMAL]));
    } else if (is(detail, "spinbutton_up") || is(detail, "spinbutton_down")) {
        kt_draw_button(&t, cr, x, y, width, height, st & ~KT_FOCUSED, kt(&style->bg[GTK_STATE_NORMAL]));
    } else if (is(detail, "spinbutton")) {
        /* The panel behind the two steppers: the entry frame covers it. */
    } else if (is(detail, "hscrollbar") || is(detail, "vscrollbar") || is(detail, "stepper")) {
        kt_draw_button(&t, cr, x, y, width, height, st & ~KT_FOCUSED, kt(&style->bg[GTK_STATE_NORMAL]));
    } else if (is(detail, "trough") && widget && GTK_IS_PROGRESS_BAR(widget)) {
        GtkProgressBarOrientation o = gtk_progress_bar_get_orientation(GTK_PROGRESS_BAR(widget));
        kt_draw_progress_trough(&t, cr, x, y, width, height, o == GTK_PROGRESS_BOTTOM_TO_TOP || o == GTK_PROGRESS_TOP_TO_BOTTOM);
        if (gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(widget)) <= 0)
            progress_text(style, cr, widget, x, y, 0, 0);
    } else if (is(detail, "bar") && widget && GTK_IS_PROGRESS_BAR(widget)) {
        GtkProgressBarOrientation o = gtk_progress_bar_get_orientation(GTK_PROGRESS_BAR(widget));
        int vertical = o == GTK_PROGRESS_BOTTOM_TO_TOP || o == GTK_PROGRESS_TOP_TO_BOTTOM;
        t.progress = kt(&style->bg[GTK_STATE_SELECTED]);
        kt_draw_progress_bar(&t, cr, x, y, width, height, vertical, anim_phase(widget));
        progress_text(style, cr, widget, x, y, width, height);
    } else if ((is(detail, "trough") || is(detail, "trough-upper") || is(detail, "trough-lower")) && widget &&
               GTK_IS_SCALE(widget)) {
        int vertical = GTK_IS_VSCALE(widget);
        GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(widget));
        double span = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) - gtk_adjustment_get_lower(adj);
        double frac = span > 0 ? (gtk_adjustment_get_value(adj) - gtk_adjustment_get_lower(adj)) / span : 0;
        if (gtk_range_get_inverted(GTK_RANGE(widget)) != (vertical != 0))
            frac = 1 - frac;
        double len = vertical ? height : width;
        if (!is(detail, "trough-lower"))
            kt_draw_scale_groove(&t, cr, x, y, width, height, vertical, vertical ? -1 : frac * len);
    } else if (is(detail, "trough")) {
        int vertical = widget && GTK_IS_VSCROLLBAR(widget);
        kt_draw_scroll_trough(&t, cr, x, y, width, height, vertical);
    } else if (is(detail, "menu")) {
        kt_draw_menu(&t, cr, x, y, width, height);
        shape_menu(widget);
    } else if (is(detail, "menuitem")) {
        if (state == GTK_STATE_PRELIGHT || state == GTK_STATE_SELECTED) {
            GtkWidget *p = widget ? gtk_widget_get_parent(widget) : NULL;
            t.selection_bg = kt(&style->bg[GTK_STATE_PRELIGHT]);
            kt_draw_menuitem(&t, cr, x, y, width, height, p && GTK_IS_MENU_BAR(p));
        }
    } else if (is(detail, "menubar") || is(detail, "toolbar") || is(detail, "handlebox") ||
               is(detail, "handlebox_bin") || is(detail, "dockitem")) {
        /* Flat, the window background shows through. */
    } else {
        cairo_destroy(cr);
        style_parent_class->draw_box(style, window, state, shadow, area, widget, detail, x, y, width, height);
        return;
    }
    cairo_destroy(cr);
}

static void ki_draw_flat_box(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                             GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                             gint x, gint y, gint width, gint height)
{
    SANITIZE_SIZE;
    if (is(detail, "tooltip")) {
        KtTheme t = g_theme;
        t.tooltip_bg = kt(&style->bg[GTK_STATE_NORMAL]);
        cairo_t *cr = begin(window, area);
        kt_draw_tooltip(&t, cr, x, y, width, height);
        cairo_destroy(cr);
        return;
    }
    if (is(detail, "checkbutton")) /* hover background of check/radio labels */
        return;
    if (detail && !strncmp(detail, "cell_", 5) && state == GTK_STATE_SELECTED) {
        KtTheme t = g_theme;
        int focused = widget && gtk_widget_has_focus(widget);
        t.selection_bg = kt(&style->base[GTK_STATE_SELECTED]);
        cairo_t *cr = begin(window, area);
        kt_draw_row_selection(&t, cr, x, y, width, height, focused);
        cairo_destroy(cr);
        return;
    }
    style_parent_class->draw_flat_box(style, window, state, shadow, area, widget, detail, x, y, width, height);
}

static void ki_draw_shadow(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                           GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                           gint x, gint y, gint width, gint height)
{
    SANITIZE_SIZE;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    if (is(detail, "entry") || (widget && GTK_IS_ENTRY(widget) && shadow != GTK_SHADOW_NONE)) {
        unsigned st = 0;
        if (state == GTK_STATE_INSENSITIVE || (widget && !gtk_widget_is_sensitive(widget)))
            st |= KT_DISABLED;
        if (widget && gtk_widget_has_focus(widget))
            st |= KT_FOCUSED;
        /* Paint the window background first: the rounded corners would
         * otherwise show whatever was there. */
        KtColor bg = kt(&style->bg[GTK_STATE_NORMAL]);
        GtkWidget *p = widget ? gtk_widget_get_parent(widget) : NULL;
        if (p)
            bg = kt(&gtk_widget_get_style(p)->bg[GTK_STATE_NORMAL]);
        cairo_set_source_rgb(cr, bg.r, bg.g, bg.b);
        cairo_rectangle(cr, x, y, width, height);
        cairo_fill(cr);
        kt_draw_entry(&t, cr, x, y, width, height, st, kt(&style->base[st & KT_DISABLED ? GTK_STATE_INSENSITIVE : GTK_STATE_NORMAL]));
    } else if (shadow == GTK_SHADOW_NONE) {
    } else if (is(detail, "menu")) {
        kt_draw_menu(&t, cr, x, y, width, height);
    } else {
        int sunken = shadow == GTK_SHADOW_IN ? 1 : shadow == GTK_SHADOW_OUT ? -1 : 0;
        if (shadow == GTK_SHADOW_ETCHED_OUT)
            sunken = 0;
        kt_draw_frame(&t, cr, x, y, width, height, sunken, kt(&style->bg[GTK_STATE_NORMAL]));
    }
    cairo_destroy(cr);
}

static void ki_draw_shadow_gap(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                               GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                               gint x, gint y, gint width, gint height, GtkPositionType side,
                               gint gap_x, gint gap_w)
{
    SANITIZE_SIZE;
    ki_draw_shadow(style, window, state, shadow, area, widget, detail, x, y, width, height);
    /* Erase the frame under the label. */
    cairo_t *cr = begin(window, area);
    gdk_cairo_set_source_color(cr, &style->bg[state]);
    switch (side) {
    case GTK_POS_TOP: cairo_rectangle(cr, x + gap_x, y, gap_w, 3); break;
    case GTK_POS_BOTTOM: cairo_rectangle(cr, x + gap_x, y + height - 3, gap_w, 3); break;
    case GTK_POS_LEFT: cairo_rectangle(cr, x, y + gap_x, 3, gap_w); break;
    case GTK_POS_RIGHT: cairo_rectangle(cr, x + width - 3, y + gap_x, 3, gap_w); break;
    }
    cairo_fill(cr);
    cairo_destroy(cr);
}

static void ki_draw_box_gap(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                            GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                            gint x, gint y, gint width, gint height, GtkPositionType side,
                            gint gap_x, gint gap_w)
{
    SANITIZE_SIZE;
    (void)shadow;
    (void)widget;
    (void)detail;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    kt_draw_tab_pane(&t, cr, x, y, width, height);
    /* Open the pane under the current tab. */
    gdk_cairo_set_source_color(cr, &style->bg[state]);
    int in = 1, d = 3;
    switch (side) {
    case GTK_POS_TOP: cairo_rectangle(cr, x + gap_x + in, y, gap_w - 2 * in, d); break;
    case GTK_POS_BOTTOM: cairo_rectangle(cr, x + gap_x + in, y + height - d, gap_w - 2 * in, d); break;
    case GTK_POS_LEFT: cairo_rectangle(cr, x, y + gap_x + in, d, gap_w - 2 * in); break;
    case GTK_POS_RIGHT: cairo_rectangle(cr, x + width - d, y + gap_x + in, d, gap_w - 2 * in); break;
    }
    cairo_fill(cr);
    cairo_destroy(cr);
}

static void ki_draw_extension(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                              GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                              gint x, gint y, gint width, gint height, GtkPositionType gap_side)
{
    SANITIZE_SIZE;
    (void)shadow;
    (void)widget;
    (void)detail;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    KtDir gap = gap_side == GTK_POS_BOTTOM ? KT_DOWN : gap_side == GTK_POS_TOP ? KT_UP
              : gap_side == GTK_POS_LEFT ? KT_LEFT : KT_RIGHT;
    /* The current tab is NORMAL, the others ACTIVE. */
    unsigned st = state == GTK_STATE_NORMAL ? KT_SELECTED : state == GTK_STATE_PRELIGHT ? KT_HOVER : 0;
    kt_draw_tab(&t, cr, x, y, width, height, gap, st);
    cairo_destroy(cr);
}

static unsigned check_state(GtkStateType state, GtkShadowType shadow)
{
    unsigned st = 0;
    if (shadow == GTK_SHADOW_IN)
        st |= KT_CHECKED;
    else if (shadow == GTK_SHADOW_ETCHED_IN)
        st |= KT_MIXED;
    if (state == GTK_STATE_PRELIGHT)
        st |= KT_HOVER;
    else if (state == GTK_STATE_ACTIVE)
        st |= KT_PRESSED;
    else if (state == GTK_STATE_INSENSITIVE)
        st |= KT_DISABLED;
    return st;
}

static void ki_draw_check(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                          GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                          gint x, gint y, gint width, gint height)
{
    (void)widget;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    unsigned st = check_state(state, shadow);
    if (is(detail, "check")) { /* GtkCheckMenuItem */
        t.base_fg = kt(&style->fg[state]);
        st |= KT_NOBOX;
    }
    kt_draw_check(&t, cr, x, y, width, height, st);
    cairo_destroy(cr);
}

static void ki_draw_option(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                           GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                           gint x, gint y, gint width, gint height)
{
    (void)widget;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    unsigned st = check_state(state, shadow);
    if (is(detail, "option")) { /* GtkRadioMenuItem */
        t.base_fg = kt(&style->fg[state]);
        st |= KT_NOBOX;
    }
    kt_draw_radio(&t, cr, x, y, width, height, st);
    cairo_destroy(cr);
}

static void ki_draw_arrow(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                          GdkRectangle *area, GtkWidget *widget, const gchar *detail, GtkArrowType arrow,
                          gboolean fill, gint x, gint y, gint width, gint height)
{
    (void)shadow;
    (void)widget;
    (void)detail;
    (void)fill;
    SANITIZE_SIZE;
    if (arrow == GTK_ARROW_NONE)
        return;
    cairo_t *cr = begin(window, area);
    KtDir dir = arrow == GTK_ARROW_UP ? KT_UP : arrow == GTK_ARROW_DOWN ? KT_DOWN : arrow == GTK_ARROW_LEFT ? KT_LEFT : KT_RIGHT;
    kt_draw_arrow(&g_theme, cr, x, y, width, height, dir, kt(&style->fg[state]));
    cairo_destroy(cr);
}

static void ki_draw_slider(GtkStyle *style, GdkWindow *window, GtkStateType state, GtkShadowType shadow,
                           GdkRectangle *area, GtkWidget *widget, const gchar *detail,
                           gint x, gint y, gint width, gint height, GtkOrientation orientation)
{
    SANITIZE_SIZE;
    KtTheme t = style_theme(style);
    cairo_t *cr = begin(window, area);
    unsigned st = state_bits(state, shadow, NULL) & ~KT_PRESSED;
    if (state == GTK_STATE_ACTIVE)
        st |= KT_PRESSED;
    if (is(detail, "hscale") || is(detail, "vscale"))
        kt_draw_scale_handle(&t, cr, x, y, width, height, st);
    else
        kt_draw_scroll_slider(&t, cr, x, y, width, height, orientation == GTK_ORIENTATION_VERTICAL, st);
    (void)widget;
    cairo_destroy(cr);
}

static void ki_draw_hline(GtkStyle *style, GdkWindow *window, GtkStateType state, GdkRectangle *area,
                          GtkWidget *widget, const gchar *detail, gint x1, gint x2, gint y)
{
    (void)style;
    (void)state;
    (void)widget;
    (void)detail;
    cairo_t *cr = begin(window, area);
    kt_draw_separator(&g_theme, cr, x1, y, x2 - x1 + 1, 2, 0);
    cairo_destroy(cr);
}

static void ki_draw_vline(GtkStyle *style, GdkWindow *window, GtkStateType state, GdkRectangle *area,
                          GtkWidget *widget, const gchar *detail, gint y1, gint y2, gint x)
{
    (void)style;
    (void)state;
    (void)widget;
    (void)detail;
    cairo_t *cr = begin(window, area);
    kt_draw_separator(&g_theme, cr, x, y1, 2, y2 - y1 + 1, 1);
    cairo_destroy(cr);
}

static void ki_draw_focus(GtkStyle *style, GdkWindow *window, GtkStateType state, GdkRectangle *area,
                          GtkWidget *widget, const gchar *detail, gint x, gint y, gint width, gint height)
{
    (void)style;
    (void)state;
    SANITIZE_SIZE;
    /* Buttons and entries show focus themselves (glow, or the classic
     * dotted rectangle inside the button). */
    if (widget && ((GTK_IS_BUTTON(widget) && !GTK_IS_CHECK_BUTTON(widget)) || GTK_IS_ENTRY(widget)))
        return;
    if (is(detail, "button") || is(detail, "entry"))
        return;
    cairo_t *cr = begin(window, area);
    kt_draw_focus(&g_theme, cr, x, y, width, height);
    cairo_destroy(cr);
}

/* Insensitive text: the base GtkStyle embosses it (a light copy offset by
 * one pixel), which only suits the classic look. */
static void ki_draw_layout(GtkStyle *style, GdkWindow *window, GtkStateType state, gboolean use_text,
                           GdkRectangle *area, GtkWidget *widget, const gchar *detail, gint x, gint y,
                           PangoLayout *layout)
{
    if (state != GTK_STATE_INSENSITIVE || g_theme.style == KT_STYLE_CLASSIC) {
        style_parent_class->draw_layout(style, window, state, use_text, area, widget, detail, x, y, layout);
        return;
    }
    GdkGC *gc = use_text ? style->text_gc[state] : style->fg_gc[state];
    if (area)
        gdk_gc_set_clip_rectangle(gc, area);
    gdk_draw_layout(window, gc, x, y, layout);
    if (area)
        gdk_gc_set_clip_rectangle(gc, NULL);
}

/* ---- RC style / style classes ---- */

static guint ki_rc_parse(GtkRcStyle *rc, GtkSettings *settings, GScanner *scanner)
{
    (void)settings;
    static GQuark scope;
    if (!scope)
        scope = g_quark_from_string("kitheme_engine");
    guint old_scope = g_scanner_set_scope(scanner, scope);

    /* GTK has already consumed the opening '{'. */
    guint token;
    for (token = g_scanner_get_next_token(scanner); token != G_TOKEN_RIGHT_CURLY;
         token = g_scanner_get_next_token(scanner)) {
        if (token == G_TOKEN_EOF)
            return G_TOKEN_RIGHT_CURLY;
        if (token != G_TOKEN_IDENTIFIER)
            continue;
        if (strcmp(scanner->value.v_identifier, "role"))
            continue;
        if (g_scanner_get_next_token(scanner) != G_TOKEN_EQUAL_SIGN)
            return G_TOKEN_EQUAL_SIGN;
        token = g_scanner_get_next_token(scanner);
        const char *v = token == G_TOKEN_STRING ? scanner->value.v_string
                      : token == G_TOKEN_IDENTIFIER ? scanner->value.v_identifier : NULL;
        if (!v)
            return G_TOKEN_STRING;
        for (int i = 0; role_names[i]; i++) {
            if (!strcmp(v, role_names[i])) {
                KI_RC_STYLE(rc)->role = (Role)i;
                KI_RC_STYLE(rc)->has_role = TRUE;
            }
        }
    }
    g_scanner_set_scope(scanner, old_scope);
    return G_TOKEN_NONE;
}

static void ki_rc_merge(GtkRcStyle *dest, GtkRcStyle *src)
{
    rc_parent_class->merge(dest, src);
    if (IS_KI_RC_STYLE(src) && KI_RC_STYLE(src)->has_role) {
        KI_RC_STYLE(dest)->role = KI_RC_STYLE(src)->role;
        KI_RC_STYLE(dest)->has_role = TRUE;
    }
}

static GtkStyle *ki_rc_create_style(GtkRcStyle *rc)
{
    (void)rc;
    return GTK_STYLE(g_object_new(ki_style_type, NULL));
}

static void ki_init_from_rc(GtkStyle *style, GtkRcStyle *rc)
{
    style_parent_class->init_from_rc(style, rc);
    theme_refresh();
    Role role = IS_KI_RC_STYLE(rc) ? KI_RC_STYLE(rc)->role : ROLE_DEFAULT;
    KI_STYLE(style)->role = role;
    apply_colors(style, rc, role);
}

static void ki_style_copy(GtkStyle *style, GtkStyle *src)
{
    style_parent_class->copy(style, src);
    KI_STYLE(style)->role = KI_STYLE(src)->role;
}

static void ki_rc_style_class_init(gpointer klass, gpointer data)
{
    (void)data;
    GtkRcStyleClass *c = GTK_RC_STYLE_CLASS(klass);
    rc_parent_class = g_type_class_peek_parent(klass);
    c->parse = ki_rc_parse;
    c->merge = ki_rc_merge;
    c->create_style = ki_rc_create_style;
}

static void ki_style_class_init(gpointer klass, gpointer data)
{
    (void)data;
    GtkStyleClass *c = GTK_STYLE_CLASS(klass);
    style_parent_class = g_type_class_peek_parent(klass);
    c->init_from_rc = ki_init_from_rc;
    c->copy = ki_style_copy;
    c->draw_box = ki_draw_box;
    c->draw_flat_box = ki_draw_flat_box;
    c->draw_shadow = ki_draw_shadow;
    c->draw_shadow_gap = ki_draw_shadow_gap;
    c->draw_box_gap = ki_draw_box_gap;
    c->draw_extension = ki_draw_extension;
    c->draw_check = ki_draw_check;
    c->draw_option = ki_draw_option;
    c->draw_arrow = ki_draw_arrow;
    c->draw_slider = ki_draw_slider;
    c->draw_hline = ki_draw_hline;
    c->draw_vline = ki_draw_vline;
    c->draw_focus = ki_draw_focus;
    c->draw_layout = ki_draw_layout;
}

/* ---- engine entry points ---- */

G_MODULE_EXPORT void theme_init(GTypeModule *module);
G_MODULE_EXPORT void theme_exit(void);
G_MODULE_EXPORT GtkRcStyle *theme_create_rc_style(void);
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);

G_MODULE_EXPORT void theme_init(GTypeModule *module)
{
    static const GTypeInfo rc_info = {
        sizeof(KiRcStyleClass), NULL, NULL, ki_rc_style_class_init, NULL, NULL, sizeof(KiRcStyle), 0, NULL, NULL,
    };
    static const GTypeInfo style_info = {
        sizeof(KiStyleClass), NULL, NULL, ki_style_class_init, NULL, NULL, sizeof(KiStyle), 0, NULL, NULL,
    };
    ki_rc_style_type = g_type_module_register_type(module, GTK_TYPE_RC_STYLE, "KiThemeRcStyle", &rc_info, 0);
    ki_style_type = g_type_module_register_type(module, GTK_TYPE_STYLE, "KiThemeStyle", &style_info, 0);
    theme_refresh();
}

G_MODULE_EXPORT void theme_exit(void)
{
}

G_MODULE_EXPORT GtkRcStyle *theme_create_rc_style(void)
{
    return GTK_RC_STYLE(g_object_new(ki_rc_style_type, NULL));
}

/* Same check every GTK2 engine makes: refuse to load into a GTK built
 * with a different ABI. Resident: window_drag leaves an emission hook and
 * signal handlers pointing into this module, so it must outlive a theme
 * switch. */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module)
{
    const gchar *err = gtk_check_version(GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION - GTK_INTERFACE_AGE);
    if (!err)
        g_module_make_resident(module);
    return err;
}
