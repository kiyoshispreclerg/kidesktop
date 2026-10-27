/* kiconf - KiTheme tab: every key of kitheme.conf, with a live preview.
 *
 * The preview is libkitheme's own kt_draw_sample() fed from the widgets,
 * so edits show up before anything is written. "Aplicar" saves
 * ~/.config/kitheme/kitheme.conf, runs `kitheme-export --notify` (theme
 * dir for GTK2/3/4, running GTK2 apps reload) and SIGHUPs kiconfd, which
 * makes running GTK3/4 apps reload too. Qt apps watch the file
 * themselves. Choosing the theme per toolkit stays in the Aparencia tab;
 * see kitheme/README.md. */
#include "../common.h"
#include "../tabs.h"

#include "../../kitheme/core/kitheme.h"

#include <limits.h>
#include <string.h>

static GtkWidget *g_preset_combo, *g_scheme_combo, *g_style_combo, *g_focus_combo, *g_ptext_combo;
static GtkWidget *g_preview, *g_status;
static GtkWidget *g_radius, *g_menu_radius, *g_relief, *g_gradient, *g_shadow;
static GtkWidget *g_hover_glow, *g_stripes, *g_animation, *g_arrows;
static GtkWidget *g_pthick, *g_sbwidth, *g_checksize;
static GtkWidget *g_simple_accent, *g_simple_bg, *g_advanced;
static char g_preset[64], g_scheme[64];
static int g_loading;
/* The two simple colors drive the 14 until one of the 14 is edited by
 * hand in "Avancado"; closing "Avancado" goes back to them. */
static int g_simple;

static const char *const style_labels[] = {N_("Suave (Oxygen)"), N_("Classico (Windows 9x)"), N_("Plano"), NULL};
static const char *const focus_labels[] = {N_("Brilho"), N_("Pontilhado"), N_("Nenhum"), NULL};
static const char *const ptext_labels[] = {N_("Quando o programa pedir"), N_("Sempre"), NULL};

typedef struct {
    const char *label;
    size_t off;
    GtkWidget *btn;
} ColorRow;

#define C(l, f) {l, offsetof(KtTheme, f), NULL}
static ColorRow g_colors[] = {
    C(N_("Fundo da janela"), window_bg), C(N_("Texto da janela"), window_fg),
    C(N_("Fundo de campos"), base_bg), C(N_("Texto de campos"), base_fg),
    C(N_("Botoes"), button_bg), C(N_("Texto dos botoes"), button_fg),
    C(N_("Selecao"), selection_bg), C(N_("Texto selecionado"), selection_fg),
    C(N_("Destaque (foco/hover)"), accent), C(N_("Barra de progresso"), progress),
    C(N_("Fundo dos menus"), menu_bg), C(N_("Texto dos menus"), menu_fg),
    C(N_("Fundo das dicas"), tooltip_bg), C(N_("Texto das dicas"), tooltip_fg),
};
#undef C
#define NCOLORS (sizeof(g_colors) / sizeof(g_colors[0]))

static KtColor *color_field(KtTheme *t, size_t off)
{
    return (KtColor *)((char *)t + off);
}

static void button_set(GtkWidget *btn, KtColor c)
{
    GdkColor g = {0, (guint16)(c.r * 65535), (guint16)(c.g * 65535), (guint16)(c.b * 65535)};
    gtk_color_button_set_color(GTK_COLOR_BUTTON(btn), &g);
}

static KtColor button_get(GtkWidget *btn)
{
    char hex[16];
    KtColor c = {0, 0, 0, 1};
    color_button_hex(btn, hex, sizeof(hex));
    kt_color_parse(hex, &c);
    return c;
}

static void set_simple(int on)
{
    g_simple = on;
    gtk_widget_set_sensitive(g_simple_accent, on);
    gtk_widget_set_sensitive(g_simple_bg, on);
}

/* Rebuilds the scheme combo from disk and selects `select` (none when it
 * is not a scheme on disk: the colors were edited by hand). */
static void refresh_schemes(const char *select)
{
    char names[64][64];
    int n = kt_list_colorschemes(names, 64), active = -1;
    int was_loading = g_loading;
    g_loading = 1;
    gtk_list_store_clear(GTK_LIST_STORE(gtk_combo_box_get_model(GTK_COMBO_BOX(g_scheme_combo))));
    for (int i = 0; i < n; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_scheme_combo), names[i]);
        if (select && !strcmp(names[i], select))
            active = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_scheme_combo), active);
    g_loading = was_loading;
}

static void select_scheme(const char *name)
{
    if (g_scheme_combo)
        refresh_schemes(name);
}

/* Widgets -> KtTheme. */
static void collect(KtTheme *t)
{
    kt_theme_defaults(t);
    snprintf(t->preset, sizeof(t->preset), "%s", g_preset);
    snprintf(t->colors, sizeof(t->colors), "%s", g_scheme);
    int s = gtk_combo_box_get_active(GTK_COMBO_BOX(g_style_combo));
    t->style = s < 0 ? KT_STYLE_SOFT : (KtStyle)s;
    for (size_t i = 0; i < NCOLORS; i++)
        *color_field(t, g_colors[i].off) = button_get(g_colors[i].btn);
    t->simple_accent = button_get(g_simple_accent);
    t->simple_bg = button_get(g_simple_bg);
    t->simple = g_simple;
    t->radius = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_radius));
    t->menu_radius = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_menu_radius));
    t->relief = gtk_range_get_value(GTK_RANGE(g_relief));
    t->gradient = gtk_range_get_value(GTK_RANGE(g_gradient));
    t->shadow = gtk_range_get_value(GTK_RANGE(g_shadow));
    int f = gtk_combo_box_get_active(GTK_COMBO_BOX(g_focus_combo));
    t->focus = f < 0 ? KT_FOCUS_GLOW : (KtFocus)f;
    t->hover_glow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_hover_glow));
    t->progress_thickness = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_pthick));
    int p = gtk_combo_box_get_active(GTK_COMBO_BOX(g_ptext_combo));
    t->progress_text = p < 0 ? KT_PTEXT_AUTO : (KtProgressText)p;
    t->progress_stripes = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_stripes));
    t->progress_animation = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_animation));
    t->scrollbar_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sbwidth));
    t->scrollbar_arrows = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_arrows));
    t->check_size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_checksize));
}

/* KtTheme -> widgets. */
static void fill(const KtTheme *t)
{
    g_loading = 1;
    snprintf(g_preset, sizeof(g_preset), "%s", t->preset);
    snprintf(g_scheme, sizeof(g_scheme), "%s", t->colors);
    select_scheme(g_scheme);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_style_combo), (int)t->style);
    for (size_t i = 0; i < NCOLORS; i++)
        button_set(g_colors[i].btn, *color_field((KtTheme *)t, g_colors[i].off));
    button_set(g_simple_accent, t->simple_accent);
    button_set(g_simple_bg, t->simple_bg);
    set_simple(t->simple);
    /* Colors edited one by one only show up in "Avancado". */
    if (!t->simple)
        gtk_expander_set_expanded(GTK_EXPANDER(g_advanced), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_radius), t->radius);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_menu_radius), t->menu_radius);
    gtk_range_set_value(GTK_RANGE(g_relief), t->relief);
    gtk_range_set_value(GTK_RANGE(g_gradient), t->gradient);
    gtk_range_set_value(GTK_RANGE(g_shadow), t->shadow);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_focus_combo), (int)t->focus);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_hover_glow), t->hover_glow);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_pthick), t->progress_thickness);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_ptext_combo), (int)t->progress_text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_stripes), t->progress_stripes);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_animation), t->progress_animation);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sbwidth), t->scrollbar_width);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_arrows), t->scrollbar_arrows);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_checksize), t->check_size);
    g_loading = 0;
    gtk_widget_queue_draw(g_preview);
}

static void changed_cb(GtkWidget *w, gpointer data);

/* A color edited by hand no longer matches the scheme in the combo. */
static void color_changed_cb(GtkWidget *w, gpointer data)
{
    if (!g_loading) {
        g_loading = 1;
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_scheme_combo), -1);
        g_loading = 0;
    }
    changed_cb(w, data);
}

/* The 14 buttons from the two simple ones. */
static void derive_colors(void)
{
    KtTheme t;
    kt_theme_defaults(&t);
    kt_colors_derive(&t, button_get(g_simple_accent), button_get(g_simple_bg));
    g_loading = 1;
    for (size_t i = 0; i < NCOLORS; i++)
        button_set(g_colors[i].btn, *color_field(&t, g_colors[i].off));
    g_loading = 0;
}

static void simple_changed_cb(GtkWidget *w, gpointer data)
{
    derive_colors();
    color_changed_cb(w, data);
}

/* One of the 14 edited by hand: the simple colors stop driving them. */
static void advanced_changed_cb(GtkWidget *w, gpointer data)
{
    if (!g_loading)
        set_simple(0);
    color_changed_cb(w, data);
}

/* Closing "Avancado" after editing the 14 goes back to the simple colors. */
static void advanced_toggled_cb(GObject *obj, GParamSpec *ps, gpointer data)
{
    (void)ps;
    if (g_loading || g_simple || gtk_expander_get_expanded(GTK_EXPANDER(obj)))
        return;
    set_simple(1);
    derive_colors();
    color_changed_cb(GTK_WIDGET(obj), data);
}

static void changed_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (g_loading)
        return;
    gtk_label_set_text(GTK_LABEL(g_status), "");
    gtk_widget_queue_draw(g_preview);
}

static gboolean preview_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)data;
    KtTheme t;
    collect(&t);
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(w));
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    kt_draw_sample(&t, cr, a.width, a.height);
    cairo_destroy(cr);
    return TRUE;
}

static void preset_cb(GtkWidget *w, gpointer data)
{
    (void)data;
    if (g_loading)
        return;
    gchar *name = gtk_combo_box_get_active_text(GTK_COMBO_BOX(w));
    if (!name)
        return;
    KtTheme cur, t;
    collect(&cur);
    if (kt_theme_load_preset(&t, name)) {
        /* A preset is only a shape: the colors stay as they are. */
        for (size_t i = 0; i < NCOLORS; i++)
            *color_field(&t, g_colors[i].off) = *color_field(&cur, g_colors[i].off);
        t.simple_accent = cur.simple_accent;
        t.simple_bg = cur.simple_bg;
        t.simple = cur.simple;
        snprintf(t.colors, sizeof(t.colors), "%s", cur.colors);
        fill(&t);
    }
    g_free(name);
    gtk_label_set_text(GTK_LABEL(g_status), "");
}

static void scheme_cb(GtkWidget *w, gpointer data)
{
    (void)data;
    if (g_loading)
        return;
    gchar *name = gtk_combo_box_get_active_text(GTK_COMBO_BOX(w));
    if (!name)
        return;
    KtTheme t;
    collect(&t);
    if (kt_theme_load_colors(&t, name))
        fill(&t);
    g_free(name);
    gtk_label_set_text(GTK_LABEL(g_status), "");
}

static void scheme_save_cb(GtkWidget *w, gpointer data)
{
    (void)data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Salvar esquema de cores"), GTK_WINDOW(gtk_widget_get_toplevel(w)),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_SAVE, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_max_length(GTK_ENTRY(entry), 40);
    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Nome do esquema:")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
        KtTheme t;
        collect(&t);
        if (*name && kt_theme_save_colors(&t, name)) {
            snprintf(g_scheme, sizeof(g_scheme), "%s", name);
            refresh_schemes(name);
            gtk_label_set_text(GTK_LABEL(g_status), _("Esquema salvo"));
        } else {
            gtk_label_set_text(GTK_LABEL(g_status), _("Nome invalido ou nao foi possivel salvar"));
        }
        g_free(name);
    }
    gtk_widget_destroy(dlg);
}

static void scheme_delete_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gchar *name = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_scheme_combo));
    if (!name)
        return;
    if (!kt_colors_is_user(name)) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Os esquemas que vem com o KiDesktop nao podem ser excluidos"));
    } else if (kt_colors_delete(name)) {
        refresh_schemes(NULL);
        gtk_label_set_text(GTK_LABEL(g_status), _("Esquema excluido"));
    }
    g_free(name);
}

/* kitheme-export from PATH, or the source tree's when kiconf runs from
 * its own directory there. */
static gchar *exporter_path(void)
{
    gchar *p = g_find_program_in_path("kitheme-export");
    if (p)
        return p;
    if (g_file_test("../kitheme/export/kitheme-export", G_FILE_TEST_IS_EXECUTABLE))
        return g_strdup("../kitheme/export/kitheme-export");
    return NULL;
}

static void apply_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    KtTheme t;
    collect(&t);
    char path[PATH_MAX];
    kt_user_conf_path(path, sizeof(path));
    if (!kt_theme_save(&t, path)) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Nao foi possivel salvar kitheme.conf"));
        return;
    }
    gchar *exporter = exporter_path();
    if (!exporter) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Salvo, mas kitheme-export nao foi encontrado"));
        return;
    }
    gchar *argv[] = {exporter, (gchar *)"--notify", NULL};
    gint status = 0;
    GError *err = NULL;
    gboolean ran = g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, &err);
    g_free(exporter);
    if (!ran || status != 0) {
        gtk_label_set_text(GTK_LABEL(g_status), _("kitheme-export falhou"));
        if (err)
            g_error_free(err);
        return;
    }
    /* GTK3/GTK4 reload when kiconfd republishes the theme name. */
    signal_daemon("kiconfd");
    gtk_label_set_text(GTK_LABEL(g_status), _("Aplicado"));
}

static GtkWidget *scale01(void)
{
    GtkWidget *s = gtk_hscale_new_with_range(0, 1, 0.05);
    gtk_scale_set_digits(GTK_SCALE(s), 2);
    gtk_widget_set_size_request(s, 140, -1);
    g_signal_connect(s, "value-changed", G_CALLBACK(changed_cb), NULL);
    return s;
}

static GtkWidget *spin(double lo, double hi, double step)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    g_signal_connect(s, "value-changed", G_CALLBACK(changed_cb), NULL);
    return s;
}

static GtkWidget *check(const char *label)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    g_signal_connect(c, "toggled", G_CALLBACK(changed_cb), NULL);
    return c;
}

static GtkWidget *labels_combo(const char *const *labels)
{
    GtkWidget *c = gtk_combo_box_new_text();
    for (int i = 0; labels[i]; i++)
        gtk_combo_box_append_text(GTK_COMBO_BOX(c), _(labels[i]));
    g_signal_connect(c, "changed", G_CALLBACK(changed_cb), NULL);
    return c;
}

/* The preview starts hidden so the tab fits kiconf's default window size;
 * showing it widens the window by the panel's width, hiding it gives the
 * width back. */
static void preview_toggle_cb(GtkWidget *btn, gpointer data)
{
    GtkWidget *panel = data;
    GtkWidget *top = gtk_widget_get_toplevel(btn);
    int cw = 0, ch = 0;
    gboolean have_win = GTK_IS_WINDOW(top);
    if (have_win)
        gtk_window_get_size(GTK_WINDOW(top), &cw, &ch);
    if (GTK_WIDGET_VISIBLE(panel)) {
        int aw = panel->allocation.width + 12;
        gtk_widget_hide(panel);
        if (have_win)
            gtk_window_resize(GTK_WINDOW(top), MAX(1, cw - aw), ch);
    } else {
        /* no_show_all keeps it hidden at startup but would also make
         * show_all skip it, so clear the flag first. */
        gtk_widget_set_no_show_all(panel, FALSE);
        gtk_widget_show_all(panel);
        if (have_win) {
            GtkRequisition req;
            gtk_widget_size_request(panel, &req);
            GtkRequisition treq;
            gtk_widget_size_request(top, &treq);
            gtk_window_resize(GTK_WINDOW(top), cw + req.width + 12, MAX(ch, treq.height));
        }
    }
}

GtkWidget *build_kitheme_tab(void)
{
    KtTheme cur;
    kt_theme_load_user(&cur);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    GtkWidget *body = gtk_hbox_new(FALSE, 12);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
    GtkWidget *left = gtk_vbox_new(FALSE, 8);

    /* Preset and overall style */
    GtkWidget *base = gtk_table_new(2, 2, FALSE);
    g_preset_combo = gtk_combo_box_new_text();
    char names[32][64];
    int n = kt_list_presets(names, 32), active = -1;
    for (int i = 0; i < n; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_preset_combo), names[i]);
        if (!strcmp(names[i], cur.preset))
            active = i;
    }
    g_loading = 1;
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_preset_combo), active);
    g_loading = 0;
    g_signal_connect(g_preset_combo, "changed", G_CALLBACK(preset_cb), NULL);
    labeled_row(base, 0, _("Partir do preset:"), g_preset_combo);
    gtk_widget_set_tooltip_text(g_preset_combo, _("Carrega a forma e os tamanhos do preset (as cores ficam como estao); os ajustes abaixo ficam por cima dele."));
    g_style_combo = labels_combo(style_labels);
    labeled_row(base, 1, _("Estilo:"), g_style_combo);
    gtk_box_pack_start(GTK_BOX(left), frame_with(_("Base"), base), FALSE, FALSE, 0);

    /* Color scheme: schemes are separate files, the buttons below edit a copy */
    GtkWidget *cbox = gtk_vbox_new(FALSE, 6);
    GtkWidget *srow = gtk_hbox_new(FALSE, 6);
    g_scheme_combo = gtk_combo_box_new_text();
    refresh_schemes(cur.colors);
    g_signal_connect(g_scheme_combo, "changed", G_CALLBACK(scheme_cb), NULL);
    gtk_widget_set_tooltip_text(g_scheme_combo, _("Esquemas de cores: os instalados com o KiDesktop e os seus. Mudar uma cor abaixo deixa a lista em branco (personalizado)."));
    a11y_name(g_scheme_combo, _("Esquema de cores"));
    GtkWidget *sbtn = gtk_button_new_with_label(_("Salvar como..."));
    g_signal_connect(sbtn, "clicked", G_CALLBACK(scheme_save_cb), NULL);
    GtkWidget *dbtn = gtk_button_new_with_label(_("Excluir"));
    g_signal_connect(dbtn, "clicked", G_CALLBACK(scheme_delete_cb), NULL);
    gtk_box_pack_start(GTK_BOX(srow), gtk_label_new(_("Esquema:")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(srow), g_scheme_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(srow), sbtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(srow), dbtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cbox), srow, FALSE, FALSE, 0);

    /* Simple colors: the 14 below are derived from these two */
    GtkWidget *simple = gtk_table_new(2, 2, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(simple), 6);
    gtk_table_set_row_spacings(GTK_TABLE(simple), 4);
    g_simple_accent = make_color_button("#000000");
    g_simple_bg = make_color_button("#000000");
    struct {
        const char *label, *tip;
        GtkWidget *btn;
    } srows[] = {
        {N_("Cor principal (destaque):"), N_("Gera a selecao, o destaque, a barra de progresso e o texto das dicas."), g_simple_accent},
        {N_("Cor de fundo:"), N_("Gera o fundo das janelas, campos, botoes e menus, e as dicas no tom oposto."), g_simple_bg},
    };
    for (int i = 0; i < 2; i++) {
        GtkWidget *l = gtk_label_new(_(srows[i].label));
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        a11y_name(srows[i].btn, _(srows[i].label));
        gtk_widget_set_tooltip_text(srows[i].btn, _(srows[i].tip));
        g_signal_connect(srows[i].btn, "color-set", G_CALLBACK(simple_changed_cb), NULL);
        gtk_table_attach(GTK_TABLE(simple), l, 0, 1, i, i + 1, GTK_FILL, 0, 0, 0);
        gtk_table_attach(GTK_TABLE(simple), srows[i].btn, 1, 2, i, i + 1, 0, 0, 0, 0);
    }
    gtk_box_pack_start(GTK_BOX(cbox), simple, FALSE, FALSE, 0);

    /* Every color, two per row, behind "Avancado" */
    GtkWidget *colors = gtk_table_new((NCOLORS + 1) / 2, 4, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(colors), 6);
    gtk_table_set_row_spacings(GTK_TABLE(colors), 4);
    for (size_t i = 0; i < NCOLORS; i++) {
        int row = (int)i / 2, col = (int)(i % 2) * 2;
        GtkWidget *l = gtk_label_new(_(g_colors[i].label));
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        g_colors[i].btn = make_color_button("#000000");
        a11y_name(g_colors[i].btn, _(g_colors[i].label));
        g_signal_connect(g_colors[i].btn, "color-set", G_CALLBACK(advanced_changed_cb), NULL);
        gtk_table_attach(GTK_TABLE(colors), l, col, col + 1, row, row + 1, GTK_FILL, 0, 0, 0);
        gtk_table_attach(GTK_TABLE(colors), g_colors[i].btn, col + 1, col + 2, row, row + 1, 0, 0, 0, 0);
    }
    g_advanced = gtk_expander_new(_("Avancado"));
    /* On the label only: on the expander it would cover the 14 buttons. */
    gtk_widget_set_tooltip_text(gtk_expander_get_label_widget(GTK_EXPANDER(g_advanced)), _("Cada uma das cores. Mudar uma delas desliga as duas cores acima; fechar esta secao volta a usa-las."));
    GtkWidget *apad = gtk_alignment_new(0, 0, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(apad), 4, 0, 12, 0);
    gtk_container_add(GTK_CONTAINER(apad), colors);
    gtk_container_add(GTK_CONTAINER(g_advanced), apad);
    g_signal_connect(g_advanced, "notify::expanded", G_CALLBACK(advanced_toggled_cb), NULL);
    gtk_box_pack_start(GTK_BOX(cbox), g_advanced, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), frame_with(_("Cores"), cbox), FALSE, FALSE, 0);

    /* Shape */
    GtkWidget *shape = gtk_table_new(7, 2, FALSE);
    g_radius = spin(0, 12, 0.5);
    labeled_row(shape, 0, _("Arredondamento (px):"), g_radius);
    g_relief = scale01();
    labeled_row(shape, 1, _("Relevo:"), g_relief);
    g_gradient = scale01();
    labeled_row(shape, 2, _("Gradiente:"), g_gradient);
    g_shadow = scale01();
    labeled_row(shape, 3, _("Sombra:"), g_shadow);
    g_focus_combo = labels_combo(focus_labels);
    labeled_row(shape, 4, _("Indicador de foco:"), g_focus_combo);
    g_menu_radius = spin(0, 16, 1);
    labeled_row(shape, 5, _("Arredondamento dos menus (px, 0 = quadrado):"), g_menu_radius);
    g_hover_glow = check(_("Brilho ao passar o mouse"));
    gtk_table_attach(GTK_TABLE(shape), g_hover_glow, 0, 2, 6, 7, GTK_FILL, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(left), frame_with(_("Forma"), shape), FALSE, FALSE, 0);

    /* Progress bars and sizes */
    GtkWidget *sizes = gtk_table_new(7, 2, FALSE);
    g_pthick = spin(4, 40, 1);
    labeled_row(sizes, 0, _("Espessura da barra de progresso:"), g_pthick);
    g_ptext_combo = labels_combo(ptext_labels);
    labeled_row(sizes, 1, _("Porcentagem dentro da barra:"), g_ptext_combo);
    g_stripes = check(_("Listras na barra (blocos no estilo classico)"));
    gtk_table_attach(GTK_TABLE(sizes), g_stripes, 0, 2, 2, 3, GTK_FILL, 0, 0, 0);
    g_animation = check(_("Animar as listras (redesenha a barra a cada 50 ms)"));
    gtk_table_attach(GTK_TABLE(sizes), g_animation, 0, 2, 3, 4, GTK_FILL, 0, 0, 0);
    g_sbwidth = spin(6, 30, 1);
    labeled_row(sizes, 4, _("Largura da barra de rolagem:"), g_sbwidth);
    g_arrows = check(_("Setas nas barras de rolagem"));
    gtk_table_attach(GTK_TABLE(sizes), g_arrows, 0, 2, 5, 6, GTK_FILL, 0, 0, 0);
    g_checksize = spin(10, 24, 1);
    labeled_row(sizes, 6, _("Tamanho de caixas de selecao:"), g_checksize);
    gtk_box_pack_start(GTK_BOX(left), frame_with(_("Barras e tamanhos"), sizes), FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), left);
    /* Wide enough for two color columns; only vertical scrolling. The
     * Apply row below stays visible whatever the scroll position. */
    gtk_widget_set_size_request(scroll, 430, 200);
    gtk_box_pack_start(GTK_BOX(body), scroll, TRUE, TRUE, 0);

    /* Preview */
    GtkWidget *pv = gtk_vbox_new(FALSE, 4);
    g_preview = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_preview, 480, 400);
    g_signal_connect(g_preview, "expose-event", G_CALLBACK(preview_expose), NULL);
    gtk_box_pack_start(GTK_BOX(pv), g_preview, FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new(_("Previa desenhada pelo proprio KiTheme. Para usa-lo, escolha \"KiTheme\"\n"
                                      "nos temas GTK e no estilo Qt da aba Aparencia."));
    gtk_misc_set_alignment(GTK_MISC(note), 0, 0);
    gtk_box_pack_start(GTK_BOX(pv), note, FALSE, FALSE, 0);
    GtkWidget *panel = frame_with(_("Previa"), pv);
    gtk_box_pack_start(GTK_BOX(body), panel, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(panel, TRUE);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    g_status = gtk_label_new("");
    GtkWidget *apply = gtk_button_new_with_label(_("Aplicar"));
    g_signal_connect(apply, "clicked", G_CALLBACK(apply_cb), NULL);
    GtkWidget *toggle = gtk_button_new_with_label(_("Visualizar/ocultar preview"));
    g_signal_connect(toggle, "clicked", G_CALLBACK(preview_toggle_cb), panel);
    gtk_box_pack_start(GTK_BOX(btnbox), g_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), toggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    fill(&cur);
    return outer;
}
