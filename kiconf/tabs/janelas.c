/* kiconf - Gerenciamento de janelas tab: kiwm.conf.
 * See kiconf.c's top doc comment for the overall design.
 *
 * kiwm has no control socket and no config-reload signal (SIGHUP means
 * "shut down", see kiwm/main.c) -- so unlike most other tabs here, this
 * one can't detect/diff live state and can't hot-reload after "Aplicar".
 * It's a plain kiwm.conf editor (same spirit as Atalhos/Paineis) plus a
 * "Reiniciar kiwm agora" button that starts a new `kiwm --replace`: the
 * running instance answers losing the ICCCM manager selection by exiting
 * on its own, so that one command is the entire restart story.
 *
 * kiwm.conf also carries key_* keybinding lines (keybind.c's own
 * generated section) that this tab knows nothing about -- "Aplicar"
 * merges its own managed keys into the file rather than rewriting it
 * whole, so those lines (and anything else this tab doesn't manage)
 * survive untouched. See MANAGED_KEYS/is_managed_key() below. */
#include "../common.h"
#include "../tabs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KIWM_CONF "kiwm.conf"

/* Every key this tab writes -- kept in one place so load, save and the
 * merge-with-foreign-lines pass in save_janelas_cb() can't drift apart. */
static const char *const MANAGED_KEYS[] = {
    "deco_bg", "deco_fg", "hide_deco_on_maximize",
    "num_desktops", "desktop_columns", "desktop_rows",
    "mod_key", "border_thickness", "border_color",
    "snap_threshold", "live_snap_resize", "outline_width", "outline_alpha",
    "resize_grip", "live_resize", "magnet_threshold", "link_resize_neighbors",
    "auto_switch_argb", "focus_follows_mouse", "appmenu_command",
    "focus_stealing_prevention", "force_unflip",
    "osd_enabled", "osd_live_preview_windows", "osd_live_preview_desktops",
    "osd_cover_switch", "osd_other_desktops", "osd_order",
    "osd_desktop_windows", "osd_output_follows_pointer",
    "new_window_output", "theme", "titlebar_layout",
    NULL,
};

static int is_managed_key(const char *key)
{
    for (int i = 0; MANAGED_KEYS[i]; i++) {
        if (!strcmp(key, MANAGED_KEYS[i])) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char deco_bg[16], deco_fg[16];
    int hide_deco_on_maximize;
    int num_desktops, desktop_columns, desktop_rows;
    char mod_key[8];
    int border_thickness;
    char border_color[16];
    int snap_threshold, live_snap_resize;
    int outline_width;
    double outline_alpha;
    int resize_grip, live_resize;
    int magnet_threshold, link_resize_neighbors;
    int auto_switch_argb, focus_follows_mouse;
    char appmenu_command[256];
    char focus_stealing_prevention[16];
    char force_unflip[8];
    int osd_enabled, osd_live_preview_windows, osd_live_preview_desktops;
    int osd_cover_switch, osd_other_desktops;
    char osd_order[8];
    int osd_desktop_windows, osd_output_follows_pointer;
    char new_window_output[16];
    char theme[NAME_LEN];
    char titlebar_layout[256];
} KiwmConfig;

/* Mirrors kiwm/config.c's apply_builtin_defaults() exactly, so a kiwm.conf
 * that doesn't exist yet shows what kiwm itself would actually start
 * with, not just zeroed widgets. */
static void kiwm_defaults(KiwmConfig *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->deco_bg, sizeof(c->deco_bg), "#000000");
    snprintf(c->deco_fg, sizeof(c->deco_fg), "#ffffff");
    c->num_desktops = 4;
    c->desktop_columns = 0;
    c->desktop_rows = 1;
    snprintf(c->mod_key, sizeof(c->mod_key), "meta");
    c->border_thickness = 0;
    snprintf(c->border_color, sizeof(c->border_color), "#000000");
    c->snap_threshold = 20;
    c->outline_width = 16;
    c->outline_alpha = 0.3;
    c->resize_grip = 12;
    c->live_resize = 1;
    c->magnet_threshold = 10;
    c->auto_switch_argb = 1;
    snprintf(c->focus_stealing_prevention, sizeof(c->focus_stealing_prevention), "none");
    snprintf(c->force_unflip, sizeof(c->force_unflip), "auto");
    c->osd_enabled = 1;
    c->osd_cover_switch = 1;
    c->osd_other_desktops = 1;
    snprintf(c->osd_order, sizeof(c->osd_order), "list");
    c->osd_desktop_windows = 1;
    snprintf(c->new_window_output, sizeof(c->new_window_output), "pointer");
    snprintf(c->theme, sizeof(c->theme), "greenxp");
    snprintf(c->titlebar_layout, sizeof(c->titlebar_layout), "icon,title,shade,minimize,maximize,close");
}

static void load_kiwm_config(KiwmConfig *c)
{
    kiwm_defaults(c);

    char path[PATH_MAX];
    resolve_path(KIWM_CONF, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);

#define SETS(field) snprintf(c->field, sizeof(c->field), "%s", val)
        if (!strcmp(key, "deco_bg")) SETS(deco_bg);
        else if (!strcmp(key, "deco_fg")) SETS(deco_fg);
        else if (!strcmp(key, "hide_deco_on_maximize")) c->hide_deco_on_maximize = atoi(val) != 0;
        else if (!strcmp(key, "num_desktops")) c->num_desktops = atoi(val);
        else if (!strcmp(key, "desktop_columns")) c->desktop_columns = atoi(val);
        else if (!strcmp(key, "desktop_rows")) c->desktop_rows = atoi(val);
        else if (!strcmp(key, "mod_key")) SETS(mod_key);
        else if (!strcmp(key, "border_thickness")) c->border_thickness = atoi(val);
        else if (!strcmp(key, "border_color")) SETS(border_color);
        else if (!strcmp(key, "snap_threshold")) c->snap_threshold = atoi(val);
        else if (!strcmp(key, "live_snap_resize")) c->live_snap_resize = atoi(val) != 0;
        else if (!strcmp(key, "outline_width")) c->outline_width = atoi(val);
        else if (!strcmp(key, "outline_alpha")) c->outline_alpha = atof(val);
        else if (!strcmp(key, "resize_grip")) c->resize_grip = atoi(val);
        else if (!strcmp(key, "live_resize")) c->live_resize = atoi(val) != 0;
        else if (!strcmp(key, "magnet_threshold")) c->magnet_threshold = atoi(val);
        else if (!strcmp(key, "link_resize_neighbors")) c->link_resize_neighbors = atoi(val) != 0;
        else if (!strcmp(key, "auto_switch_argb")) c->auto_switch_argb = atoi(val) != 0;
        else if (!strcmp(key, "focus_follows_mouse")) c->focus_follows_mouse = atoi(val) != 0;
        else if (!strcmp(key, "appmenu_command")) SETS(appmenu_command);
        else if (!strcmp(key, "focus_stealing_prevention")) SETS(focus_stealing_prevention);
        else if (!strcmp(key, "force_unflip")) SETS(force_unflip);
        else if (!strcmp(key, "osd_enabled")) c->osd_enabled = atoi(val) != 0;
        else if (!strcmp(key, "osd_live_preview_windows")) c->osd_live_preview_windows = atoi(val) != 0;
        else if (!strcmp(key, "osd_live_preview_desktops")) c->osd_live_preview_desktops = atoi(val) != 0;
        else if (!strcmp(key, "osd_cover_switch")) c->osd_cover_switch = atoi(val) != 0;
        else if (!strcmp(key, "osd_other_desktops")) c->osd_other_desktops = atoi(val) != 0;
        else if (!strcmp(key, "osd_order")) SETS(osd_order);
        else if (!strcmp(key, "osd_desktop_windows")) c->osd_desktop_windows = atoi(val) != 0;
        else if (!strcmp(key, "osd_output_follows_pointer")) c->osd_output_follows_pointer = atoi(val) != 0;
        else if (!strcmp(key, "new_window_output")) SETS(new_window_output);
        else if (!strcmp(key, "theme")) SETS(theme);
        else if (!strcmp(key, "titlebar_layout")) SETS(titlebar_layout);
#undef SETS
    }
    fclose(f);
}

/* Widgets */
static GtkWidget *g_status_label;
static GtkWidget *g_deco_bg_btn, *g_deco_fg_btn, *g_hide_deco_chk;
static GtkWidget *g_theme_entry, *g_titlebar_entry;
static GtkWidget *g_border_thick_spin, *g_border_color_btn, *g_grip_spin, *g_live_resize_chk;
static GtkWidget *g_snap_spin, *g_live_snap_chk, *g_outline_w_spin, *g_outline_a_spin;
static GtkWidget *g_magnet_spin, *g_link_resize_chk;
static GtkWidget *g_modkey_combo, *g_ffm_chk, *g_fsp_combo, *g_force_unflip_combo;
static GtkWidget *g_auto_argb_chk, *g_new_win_combo, *g_appmenu_entry;
static GtkWidget *g_ndesk_spin, *g_dcols_spin, *g_drows_spin;
static GtkWidget *g_osd_enabled_chk, *g_osd_live_win_chk, *g_osd_live_desk_chk;
static GtkWidget *g_osd_pointer_chk, *g_osd_order_combo, *g_osd_deskwin_chk;
static GtkWidget *g_osd_cover_chk, *g_osd_other_chk;

static void refresh_status(void)
{
    gtk_label_set_text(GTK_LABEL(g_status_label),
                        process_running("kiwm")
                            ? "kiwm esta em execucao."
                            : "kiwm nao esta em execucao -- estas configuracoes valem para a proxima vez que ele iniciar.");
}

static const char *const MOD_KEY_OPTS[] = {"meta", "alt", NULL};
static const char *const FSP_OPTS[] = {"none", "low", "normal", "high", "extreme", NULL};
static const char *const FORCE_UNFLIP_OPTS[] = {"auto", "always", "never", NULL};
static const char *const NEW_WIN_OPTS[] = {"pointer", "largest", NULL};
static const char *const OSD_ORDER_OPTS[] = {"list", "mru", NULL};

static void save_janelas_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    KiwmConfig c;
    memset(&c, 0, sizeof(c));
    color_button_hex(g_deco_bg_btn, c.deco_bg, sizeof(c.deco_bg));
    color_button_hex(g_deco_fg_btn, c.deco_fg, sizeof(c.deco_fg));
    c.hide_deco_on_maximize = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_hide_deco_chk));
    snprintf(c.theme, sizeof(c.theme), "%s", gtk_entry_get_text(GTK_ENTRY(g_theme_entry)));
    snprintf(c.titlebar_layout, sizeof(c.titlebar_layout), "%s", gtk_entry_get_text(GTK_ENTRY(g_titlebar_entry)));

    c.border_thickness = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_border_thick_spin));
    color_button_hex(g_border_color_btn, c.border_color, sizeof(c.border_color));
    c.resize_grip = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_grip_spin));
    c.live_resize = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_live_resize_chk));

    c.snap_threshold = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_snap_spin));
    c.live_snap_resize = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_live_snap_chk));
    c.outline_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_outline_w_spin));
    c.outline_alpha = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_outline_a_spin));
    c.magnet_threshold = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_magnet_spin));
    c.link_resize_neighbors = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_link_resize_chk));

    snprintf(c.mod_key, sizeof(c.mod_key), "%s", combo_text(g_modkey_combo, MOD_KEY_OPTS));
    c.focus_follows_mouse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_ffm_chk));
    snprintf(c.focus_stealing_prevention, sizeof(c.focus_stealing_prevention), "%s", combo_text(g_fsp_combo, FSP_OPTS));
    snprintf(c.force_unflip, sizeof(c.force_unflip), "%s", combo_text(g_force_unflip_combo, FORCE_UNFLIP_OPTS));
    c.auto_switch_argb = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_auto_argb_chk));
    snprintf(c.new_window_output, sizeof(c.new_window_output), "%s", combo_text(g_new_win_combo, NEW_WIN_OPTS));
    snprintf(c.appmenu_command, sizeof(c.appmenu_command), "%s", gtk_entry_get_text(GTK_ENTRY(g_appmenu_entry)));

    c.num_desktops = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_ndesk_spin));
    c.desktop_columns = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dcols_spin));
    c.desktop_rows = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_drows_spin));

    c.osd_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_enabled_chk));
    c.osd_live_preview_windows = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_live_win_chk));
    c.osd_live_preview_desktops = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_live_desk_chk));
    c.osd_output_follows_pointer = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_pointer_chk));
    snprintf(c.osd_order, sizeof(c.osd_order), "%s", combo_text(g_osd_order_combo, OSD_ORDER_OPTS));
    c.osd_desktop_windows = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_deskwin_chk));
    c.osd_cover_switch = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_cover_chk));
    c.osd_other_desktops = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_osd_other_chk));

    char path[PATH_MAX];
    resolve_path(KIWM_CONF, path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    fprintf(out, "# kiwm configuration -- window/border/behavior settings written by kiconf.\n");
    fprintf(out, "# Any key not managed here (key_* shortcuts included) is kept as-is below.\n");
    fprintf(out, "# See kiwm/config.c for the full key=value reference.\n\n");
    fprintf(out, "deco_bg=%s\n", c.deco_bg);
    fprintf(out, "deco_fg=%s\n", c.deco_fg);
    fprintf(out, "hide_deco_on_maximize=%d\n", c.hide_deco_on_maximize);
    fprintf(out, "num_desktops=%d\n", c.num_desktops);
    fprintf(out, "desktop_columns=%d\n", c.desktop_columns);
    fprintf(out, "desktop_rows=%d\n", c.desktop_rows);
    fprintf(out, "mod_key=%s\n", c.mod_key);
    fprintf(out, "border_thickness=%d\n", c.border_thickness);
    fprintf(out, "border_color=%s\n", c.border_color);
    fprintf(out, "snap_threshold=%d\n", c.snap_threshold);
    fprintf(out, "live_snap_resize=%d\n", c.live_snap_resize);
    fprintf(out, "outline_width=%d\n", c.outline_width);
    fprintf_double(out, "outline_alpha", c.outline_alpha, 2);
    fprintf(out, "resize_grip=%d\n", c.resize_grip);
    fprintf(out, "live_resize=%d\n", c.live_resize);
    fprintf(out, "magnet_threshold=%d\n", c.magnet_threshold);
    fprintf(out, "link_resize_neighbors=%d\n", c.link_resize_neighbors);
    fprintf(out, "auto_switch_argb=%d\n", c.auto_switch_argb);
    fprintf(out, "focus_follows_mouse=%d\n", c.focus_follows_mouse);
    fprintf(out, "appmenu_command=%s\n", c.appmenu_command);
    fprintf(out, "focus_stealing_prevention=%s\n", c.focus_stealing_prevention);
    fprintf(out, "force_unflip=%s\n", c.force_unflip);
    fprintf(out, "osd_enabled=%d\n", c.osd_enabled);
    fprintf(out, "osd_live_preview_windows=%d\n", c.osd_live_preview_windows);
    fprintf(out, "osd_live_preview_desktops=%d\n", c.osd_live_preview_desktops);
    fprintf(out, "osd_cover_switch=%d\n", c.osd_cover_switch);
    fprintf(out, "osd_other_desktops=%d\n", c.osd_other_desktops);
    fprintf(out, "osd_order=%s\n", c.osd_order);
    fprintf(out, "osd_desktop_windows=%d\n", c.osd_desktop_windows);
    fprintf(out, "osd_output_follows_pointer=%d\n", c.osd_output_follows_pointer);
    fprintf(out, "new_window_output=%s\n", c.new_window_output);
    fprintf(out, "theme=%s\n", c.theme);
    fprintf(out, "titlebar_layout=%s\n", c.titlebar_layout);

    /* Carry over every line this tab doesn't manage -- key_* shortcuts
     * above all -- from whatever kiwm.conf already had. */
    FILE *in = fopen(path, "r");
    if (in) {
        fprintf(out, "\n");
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", line);
            char *l = trim(buf);
            if (!*l || *l == '#') {
                continue;
            }
            char *eq = strchr(l, '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            if (is_managed_key(trim(l))) {
                continue;
            }
            fprintf(out, "%s\n", line);
        }
        fclose(in);
    }

    fclose(out);
    rename(tmp, path);

    refresh_status();
}

static void restart_kiwm_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    spawn_replace("kiwm");
    refresh_status();
}

GtkWidget *build_janelas_tab(void)
{
    KiwmConfig c;
    load_kiwm_config(&c);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_status_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_status_label, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *content = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 2);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    /* Decoracao */
    GtkWidget *deco_table = gtk_table_new(4, 2, FALSE);
    g_deco_bg_btn = labeled_row(deco_table, 0, "Cor de fundo:", make_color_button(c.deco_bg));
    g_deco_fg_btn = labeled_row(deco_table, 1, "Cor do texto:", make_color_button(c.deco_fg));
    g_hide_deco_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_hide_deco_chk), c.hide_deco_on_maximize);
    labeled_row(deco_table, 2, "Ocultar decoracao ao maximizar:", g_hide_deco_chk);
    g_theme_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_theme_entry), c.theme);
    labeled_row(deco_table, 3, "Tema:", g_theme_entry);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Decoracao", deco_table), FALSE, FALSE, 0);

    GtkWidget *titlebar_table = gtk_table_new(1, 2, FALSE);
    g_titlebar_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_titlebar_entry), c.titlebar_layout);
    labeled_row(titlebar_table, 0, "Elementos da barra de titulo:", g_titlebar_entry);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Barra de titulo (lista separada por virgulas: icon,title,"
                                                 "shade,minimize,maximize,close,keep_above,keep_all_desktops,appmenu)",
                                                 titlebar_table),
                        FALSE, FALSE, 0);

    /* Borda e redimensionamento */
    GtkWidget *border_table = gtk_table_new(4, 2, FALSE);
    g_border_thick_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_border_thick_spin), c.border_thickness);
    labeled_row(border_table, 0, "Espessura da borda:", g_border_thick_spin);
    g_border_color_btn = labeled_row(border_table, 1, "Cor da borda:", make_color_button(c.border_color));
    g_grip_spin = gtk_spin_button_new_with_range(0, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_grip_spin), c.resize_grip);
    labeled_row(border_table, 2, "Faixa de redimensionamento (px):", g_grip_spin);
    g_live_resize_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_live_resize_chk), c.live_resize);
    labeled_row(border_table, 3, "Redimensionar ao vivo:", g_live_resize_chk);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Borda e redimensionamento", border_table), FALSE, FALSE, 0);

    /* Encaixe (snap) */
    GtkWidget *snap_table = gtk_table_new(6, 2, FALSE);
    g_snap_spin = gtk_spin_button_new_with_range(0, 200, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_snap_spin), c.snap_threshold);
    labeled_row(snap_table, 0, "Distancia para encaixar na borda da tela (px):", g_snap_spin);
    g_live_snap_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_live_snap_chk), c.live_snap_resize);
    labeled_row(snap_table, 1, "Redimensionar ao vivo durante o encaixe:", g_live_snap_chk);
    g_outline_w_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_outline_w_spin), c.outline_width);
    labeled_row(snap_table, 2, "Espessura do contorno de pre-visualizacao (px):", g_outline_w_spin);
    g_outline_a_spin = gtk_spin_button_new_with_range(0.0, 1.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_outline_a_spin), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_outline_a_spin), c.outline_alpha);
    labeled_row(snap_table, 3, "Opacidade do contorno (com compositor):", g_outline_a_spin);
    g_magnet_spin = gtk_spin_button_new_with_range(0, 200, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_magnet_spin), c.magnet_threshold);
    labeled_row(snap_table, 4, "Distancia para encaixar em outra janela (px):", g_magnet_spin);
    g_link_resize_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_link_resize_chk), c.link_resize_neighbors);
    labeled_row(snap_table, 5, "Redimensionar vizinhas junto:", g_link_resize_chk);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Encaixe (snap)", snap_table), FALSE, FALSE, 0);

    /* Comportamento */
    GtkWidget *behavior_table = gtk_table_new(7, 2, FALSE);
    g_modkey_combo = make_options_combo(MOD_KEY_OPTS, c.mod_key);
    labeled_row(behavior_table, 0, "Modificador (mover/redimensionar):", g_modkey_combo);
    g_ffm_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ffm_chk), c.focus_follows_mouse);
    labeled_row(behavior_table, 1, "Foco segue o mouse:", g_ffm_chk);
    g_fsp_combo = make_options_combo(FSP_OPTS, c.focus_stealing_prevention);
    labeled_row(behavior_table, 2, "Prevencao de roubo de foco:", g_fsp_combo);
    g_force_unflip_combo = make_options_combo(FORCE_UNFLIP_OPTS, c.force_unflip);
    labeled_row(behavior_table, 3, "Repintar apos tela cheia:", g_force_unflip_combo);
    g_auto_argb_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_auto_argb_chk), c.auto_switch_argb);
    labeled_row(behavior_table, 4, "Trocar para ARGB automaticamente com compositor:", g_auto_argb_chk);
    g_new_win_combo = make_options_combo(NEW_WIN_OPTS, c.new_window_output);
    labeled_row(behavior_table, 5, "Saida para novas janelas:", g_new_win_combo);
    g_appmenu_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_appmenu_entry), c.appmenu_command);
    labeled_row(behavior_table, 6, "Comando do botao de menu (appmenu):", g_appmenu_entry);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Comportamento", behavior_table), FALSE, FALSE, 0);

    /* Areas de trabalho */
    GtkWidget *desk_table = gtk_table_new(3, 2, FALSE);
    g_ndesk_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ndesk_spin), c.num_desktops);
    labeled_row(desk_table, 0, "Numero de areas de trabalho (por saida):", g_ndesk_spin);
    g_dcols_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dcols_spin), c.desktop_columns);
    labeled_row(desk_table, 1, "Colunas na grade (0 = automatico):", g_dcols_spin);
    g_drows_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_drows_spin), c.desktop_rows);
    labeled_row(desk_table, 2, "Linhas na grade (0 = automatico):", g_drows_spin);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Areas de trabalho", desk_table), FALSE, FALSE, 0);

    /* Alternador (Alt+Tab / ModKey+Tab) */
    GtkWidget *osd_table = gtk_table_new(8, 2, FALSE);
    g_osd_enabled_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_enabled_chk), c.osd_enabled);
    labeled_row(osd_table, 0, "Mostrar sobreposicao ao segurar o atalho:", g_osd_enabled_chk);
    g_osd_live_win_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_live_win_chk), c.osd_live_preview_windows);
    labeled_row(osd_table, 1, "Pre-visualizar janela ao vivo:", g_osd_live_win_chk);
    g_osd_live_desk_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_live_desk_chk), c.osd_live_preview_desktops);
    labeled_row(osd_table, 2, "Pre-visualizar area de trabalho ao vivo:", g_osd_live_desk_chk);
    g_osd_pointer_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_pointer_chk), c.osd_output_follows_pointer);
    labeled_row(osd_table, 3, "Abrir sempre na saida do ponteiro:", g_osd_pointer_chk);
    g_osd_order_combo = make_options_combo(OSD_ORDER_OPTS, c.osd_order);
    labeled_row(osd_table, 4, "Ordem da lista de janelas:", g_osd_order_combo);
    g_osd_deskwin_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_deskwin_chk), c.osd_desktop_windows);
    labeled_row(osd_table, 5, "Desenhar janelas dentro de cada area de trabalho:", g_osd_deskwin_chk);
    g_osd_cover_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_cover_chk), c.osd_cover_switch);
    labeled_row(osd_table, 6, "Usar efeito cover-switch do compositor, se houver:", g_osd_cover_chk);
    g_osd_other_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_osd_other_chk), c.osd_other_desktops);
    labeled_row(osd_table, 7, "Listar janelas de outras areas de trabalho:", g_osd_other_chk);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Alternador (Alt+Tab / ModKey+Tab)", osd_table), FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *restart_btn = gtk_button_new_with_label("Reiniciar kiwm agora");
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar (grava kiwm.conf)");
    g_signal_connect(restart_btn, "clicked", G_CALLBACK(restart_kiwm_cb), NULL);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(save_janelas_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), restart_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    refresh_status();
    return outer;
}
