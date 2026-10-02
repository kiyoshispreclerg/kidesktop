/* kiconf - Acessibilidade tab: XKB AccessX toggles, text scale, cursor
 * size, high contrast, and kisession's assistive-technology services.
 *
 * Keyboard toggles and text scale go to kiconfd-a11y.conf, which kiconfd
 * applies (XkbSetControls / Xft DPI) at session start and on SIGHUP --
 * see kiconfd/README.md. Cursor size and high contrast are kiconfd.conf
 * keys the Aparencia tab also owns; this tab only rewrites those keys in
 * place, so whatever else Aparencia saved stays. High contrast keeps the
 * values it replaced as saved_* lines in kiconfd-a11y.conf, and turning
 * it off puts them back. */
#include "../common.h"
#include "../tabs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static GtkWidget *g_sticky_chk, *g_sticky_two_chk, *g_slow_chk, *g_slow_spin, *g_bounce_chk, *g_bounce_spin;
static GtkWidget *g_mouse_chk, *g_gestures_chk;
static GtkWidget *g_scale_spin, *g_cursor_spin, *g_hc_chk;
static GtkWidget *g_atspi_chk, *g_orca_chk;

/* kiconfd.conf keys high contrast replaces, and what it sets them to. */
static const char *const HC_KEYS[] = {
    "gtk2_theme", "gtk3_theme", "gtk4_theme", "theme", "color_bg", "color_fg",
    "color_base", "color_accent", "color_selection_bg", "color_selection_fg", NULL,
};
static const char *const HC_VALUES[] = {
    "HighContrast", "HighContrast", "HighContrast", "highcontrast", "#000000", "#ffffff",
    "#000000", "#ffff00", "#ffff00", "#000000", NULL,
};

/* ---- "key = value" files -------------------------------------------- */

#define KV_MAX 64

typedef struct {
    char key[64];
    char val[256];
} Kv;

typedef struct {
    Kv kv[KV_MAX];
    int n;
} KvFile;

static void kv_load(const char *name, KvFile *f)
{
    f->n = 0;
    char path[PATH_MAX];
    resolve_path(name, path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp) && f->n < KV_MAX) {
        char *l = trim(line);
        char *eq = strchr(l, '=');
        if (!*l || *l == '#' || !eq) {
            continue;
        }
        *eq = '\0';
        snprintf(f->kv[f->n].key, sizeof(f->kv[f->n].key), "%s", trim(l));
        snprintf(f->kv[f->n].val, sizeof(f->kv[f->n].val), "%s", trim(eq + 1));
        f->n++;
    }
    fclose(fp);
}

static const char *kv_find(const KvFile *f, const char *key)
{
    for (int i = 0; i < f->n; i++) {
        if (!strcmp(f->kv[i].key, key)) {
            return f->kv[i].val;
        }
    }
    return NULL;
}

static void kv_set(KvFile *f, const char *key, const char *val)
{
    for (int i = 0; i < f->n; i++) {
        if (!strcmp(f->kv[i].key, key)) {
            snprintf(f->kv[i].val, sizeof(f->kv[i].val), "%s", val);
            return;
        }
    }
    if (f->n < KV_MAX) {
        snprintf(f->kv[f->n].key, sizeof(f->kv[f->n].key), "%s", key);
        snprintf(f->kv[f->n].val, sizeof(f->kv[f->n].val), "%s", val);
        f->n++;
    }
}

static void kv_unset(KvFile *f, const char *key)
{
    for (int i = 0; i < f->n; i++) {
        if (!strcmp(f->kv[i].key, key)) {
            f->kv[i] = f->kv[--f->n];
            return;
        }
    }
}

static int kv_int(const KvFile *f, const char *key, int def)
{
    const char *v = kv_find(f, key);
    return v ? atoi(v) : def;
}

static void kv_save(const char *name, const KvFile *f, const char *header)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    resolve_path(name, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(fp, "%s", header);
    for (int i = 0; i < f->n; i++) {
        fprintf(fp, "%s = %s\n", f->kv[i].key, f->kv[i].val);
    }
    fclose(fp);
    if (rename(tmp, path) != 0) {
        g_warning("kiconf: could not save '%s': %s", path, strerror(errno));
    }
}

/* ---- the high-contrast theme files ------------------------------------ */

/* Written on enable rather than shipped: one KiDesktop theme (kiwm
 * titlebars, xispanel bars) and one engine-less GTK2 theme. GTK3/4 have
 * a built-in "HighContrast"; the GTK2 one is named the same because
 * kiconfd's XSETTINGS carries the GTK3 name to GTK2 apps as well. */
static const char HC_COLORS[] =
    "# High-contrast KiDesktop theme, written by kiconf's Acessibilidade tab.\n"
    "bg_active=#000000\nbg_inactive=#000000\nfg_active=#ffffff\nfg_inactive=#d0d0d0\n"
    "border_active=#ffff00\nborder_inactive=#ffffff\n"
    "button_bg_active=#000000\nbutton_bg_inactive=#000000\nbutton_fg_active=#ffffff\nbutton_fg_inactive=#d0d0d0\n"
    "font_size=15\nfont_weight=bold\nborder_radius=0\nround_maximized=0\n";

static const char HC_GTKRC[] =
    "# HighContrast for GTK2 (no engine), written by kiconf's Acessibilidade tab.\n"
    "gtk-color-scheme = \"bg_color:#000000\\nfg_color:#ffffff\\nbase_color:#000000\\ntext_color:#ffffff\\n"
    "selected_bg_color:#ffff00\\nselected_fg_color:#000000\\ntooltip_bg_color:#000000\\ntooltip_fg_color:#ffffff\"\n"
    "style \"kihc-default\" {\n"
    "  xthickness = 2\n  ythickness = 2\n"
    "  GtkWidget::focus-line-width = 3\n  GtkWidget::focus-line-pattern = \"\\1\"\n"
    "  GtkRange::slider-width = 18\n  GtkCheckButton::indicator-size = 16\n"
    "  bg[NORMAL] = \"#000000\"\n  bg[PRELIGHT] = \"#303030\"\n  bg[ACTIVE] = \"#202020\"\n"
    "  bg[SELECTED] = \"#ffff00\"\n  bg[INSENSITIVE] = \"#000000\"\n"
    "  fg[NORMAL] = \"#ffffff\"\n  fg[PRELIGHT] = \"#ffffff\"\n  fg[ACTIVE] = \"#ffffff\"\n"
    "  fg[SELECTED] = \"#000000\"\n  fg[INSENSITIVE] = \"#a0a0a0\"\n"
    "  base[NORMAL] = \"#000000\"\n  base[PRELIGHT] = \"#303030\"\n  base[ACTIVE] = \"#ffff00\"\n"
    "  base[SELECTED] = \"#ffff00\"\n  base[INSENSITIVE] = \"#000000\"\n"
    "  text[NORMAL] = \"#ffffff\"\n  text[PRELIGHT] = \"#ffffff\"\n  text[ACTIVE] = \"#000000\"\n"
    "  text[SELECTED] = \"#000000\"\n  text[INSENSITIVE] = \"#a0a0a0\"\n"
    "}\nclass \"GtkWidget\" style \"kihc-default\"\n"
    "style \"kihc-hover\" = \"kihc-default\" {\n"
    "  bg[PRELIGHT] = \"#ffff00\"\n  fg[PRELIGHT] = \"#000000\"\n  bg[ACTIVE] = \"#ffff00\"\n  fg[ACTIVE] = \"#000000\"\n"
    "}\nwidget_class \"*Button*\" style \"kihc-hover\"\nwidget_class \"*MenuItem*\" style \"kihc-hover\"\n"
    "# A checked check/radio button is in the ACTIVE state too; keep its label readable.\n"
    "style \"kihc-toggle\" = \"kihc-default\" {\n"
    "  bg[PRELIGHT] = \"#303030\"\n  fg[PRELIGHT] = \"#ffffff\"\n  bg[ACTIVE] = \"#000000\"\n  fg[ACTIVE] = \"#ffffff\"\n"
    "}\nwidget_class \"*CheckButton*\" style \"kihc-toggle\"\nwidget_class \"*RadioButton*\" style \"kihc-toggle\"\n"
    "# The default engine shades frames from bg: black would hide every border.\n"
    "style \"kihc-frame\" = \"kihc-default\" {\n  bg[NORMAL] = \"#ffffff\"\n  bg[INSENSITIVE] = \"#a0a0a0\"\n}\n"
    "class \"GtkEntry\" style \"kihc-frame\"\nclass \"GtkFrame\" style \"kihc-frame\"\n"
    "class \"GtkScrolledWindow\" style \"kihc-frame\"\n";

static int write_file(const char *path, const char *content)
{
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GError *err = NULL;
    if (!g_file_set_contents(path, content, -1, &err)) {
        g_warning("kiconf: could not write '%s': %s", path, err ? err->message : "?");
        g_clear_error(&err);
        return 0;
    }
    return 1;
}

static void write_hc_themes(void)
{
    const char *home = g_get_home_dir();
    /* $HOME/.local/share, not XDG_DATA_HOME: the path kiwm/xispanel search. */
    char *data = g_build_filename(home, ".local", "share", "kidesktop", "themes", "highcontrast", "colors", NULL);
    char *gtkrc = g_build_filename(home, ".themes", "HighContrast", "gtk-2.0", "gtkrc", NULL);
    write_file(data, HC_COLORS);
    write_file(gtkrc, HC_GTKRC);
    g_free(data);
    g_free(gtkrc);
}

/* ---- kisession services ----------------------------------------------- */

static int kisession_index(const char *name)
{
    for (int i = 0; i < N_KISESSION_SERVICES; i++) {
        if (!strcmp(KISESSION_SERVICES[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* ---- Aplicar ---------------------------------------------------------- */

static int active(GtkWidget *chk)
{
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk)) ? 1 : 0;
}

static void apply_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    KvFile a11y, conf;
    kv_load("kiconfd-a11y.conf", &a11y);
    kv_load("kiconfd.conf", &conf);
    char buf[32];

    kv_set(&a11y, "sticky_keys", active(g_sticky_chk) ? "1" : "0");
    kv_set(&a11y, "sticky_keys_two_key_off", active(g_sticky_two_chk) ? "1" : "0");
    kv_set(&a11y, "slow_keys", active(g_slow_chk) ? "1" : "0");
    snprintf(buf, sizeof(buf), "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_slow_spin)));
    kv_set(&a11y, "slow_keys_delay", buf);
    kv_set(&a11y, "bounce_keys", active(g_bounce_chk) ? "1" : "0");
    snprintf(buf, sizeof(buf), "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_bounce_spin)));
    kv_set(&a11y, "bounce_keys_delay", buf);
    kv_set(&a11y, "mouse_keys", active(g_mouse_chk) ? "1" : "0");
    kv_set(&a11y, "keyboard_gestures", active(g_gestures_chk) ? "1" : "0");
    g_ascii_formatd(buf, sizeof(buf), "%.2f", gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_scale_spin)));
    kv_set(&a11y, "text_scale", buf);

    snprintf(buf, sizeof(buf), "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_cursor_spin)));
    kv_set(&conf, "cursor_size", buf);

    int hc_was = kv_int(&a11y, "high_contrast", 0), hc = active(g_hc_chk);
    if (hc) {
        write_hc_themes(); /* every time, so a newer kiconf's theme replaces an older copy */
    }
    if (hc && !hc_was) {
        for (int i = 0; HC_KEYS[i]; i++) {
            char saved[96];
            snprintf(saved, sizeof(saved), "saved_%s", HC_KEYS[i]);
            const char *cur = kv_find(&conf, HC_KEYS[i]);
            kv_set(&a11y, saved, cur ? cur : "");
            kv_set(&conf, HC_KEYS[i], HC_VALUES[i]);
        }
    } else if (!hc && hc_was) {
        for (int i = 0; HC_KEYS[i]; i++) {
            char saved[96];
            snprintf(saved, sizeof(saved), "saved_%s", HC_KEYS[i]);
            const char *old = kv_find(&a11y, saved);
            if (old) {
                char val[256];
                snprintf(val, sizeof(val), "%s", old); /* kv_unset() below reuses the slot */
                kv_set(&conf, HC_KEYS[i], val);
                kv_unset(&a11y, saved);
            }
        }
    }
    kv_set(&a11y, "high_contrast", hc ? "1" : "0");

    kv_save("kiconfd-a11y.conf", &a11y,
            "# kiconfd accessibility settings -- generated by kiconf's Acessibilidade tab.\n");
    kv_save("kiconfd.conf", &conf, "# kiconfd config\n");
    signal_daemon("kiconfd");

    KisessionConfig ks;
    kisession_load(&ks);
    int ia = kisession_index("a11y"), io = kisession_index("screenreader");
    int changed = 0;
    if (ia >= 0 && ks.enabled[ia] != active(g_atspi_chk)) {
        ks.enabled[ia] = active(g_atspi_chk);
        changed = 1;
    }
    if (io >= 0 && ks.enabled[io] != active(g_orca_chk)) {
        ks.enabled[io] = active(g_orca_chk);
        changed = 1;
    }
    if (changed) {
        kisession_save(&ks);
    }
}

/* ---- tab ---------------------------------------------------------------- */

static GtkWidget *check(const char *label, int on)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    return c;
}

static void attach_full(GtkWidget *table, int row, GtkWidget *w)
{
    gtk_table_attach(GTK_TABLE(table), w, 0, 2, row, row + 1, GTK_FILL, GTK_FILL, 4, 2);
}

static void sync_sensitive(GtkWidget *chk, gpointer target)
{
    gtk_widget_set_sensitive(GTK_WIDGET(target), active(chk));
}

GtkWidget *build_acessibilidade_tab(void)
{
    KvFile a11y, conf;
    kv_load("kiconfd-a11y.conf", &a11y);
    kv_load("kiconfd.conf", &conf);
    KisessionConfig ks;
    kisession_load(&ks);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);

    /* Keyboard */
    GtkWidget *kt = gtk_table_new(8, 2, FALSE);
    g_sticky_chk = check(_("Teclas de aderencia: Shift, Ctrl e Alt ficam pressionados ate a proxima tecla"),
                         kv_int(&a11y, "sticky_keys", 0));
    attach_full(kt, 0, g_sticky_chk);
    g_sticky_two_chk = check(_("Desativar a aderencia ao pressionar duas teclas juntas"),
                             kv_int(&a11y, "sticky_keys_two_key_off", 1));
    attach_full(kt, 1, g_sticky_two_chk);
    g_slow_chk = check(_("Teclas lentas: so aceitar teclas mantidas pressionadas"), kv_int(&a11y, "slow_keys", 0));
    attach_full(kt, 2, g_slow_chk);
    g_slow_spin = gtk_spin_button_new_with_range(50, 2000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_slow_spin), kv_int(&a11y, "slow_keys_delay", 300));
    labeled_row(kt, 3, _("Tempo para aceitar (ms):"), g_slow_spin);
    g_bounce_chk = check(_("Teclas de repercussao: ignorar toques repetidos rapidos"), kv_int(&a11y, "bounce_keys", 0));
    attach_full(kt, 4, g_bounce_chk);
    g_bounce_spin = gtk_spin_button_new_with_range(50, 2000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_bounce_spin), kv_int(&a11y, "bounce_keys_delay", 300));
    labeled_row(kt, 5, _("Ignorar repeticoes por (ms):"), g_bounce_spin);
    g_mouse_chk = check(_("Teclas do mouse: mover o ponteiro com o teclado numerico"), kv_int(&a11y, "mouse_keys", 0));
    attach_full(kt, 6, g_mouse_chk);
    g_gestures_chk = check(_("Ativar pelo teclado: Shift cinco vezes (aderencia) ou mantido 8 s (lentas)"),
                           kv_int(&a11y, "keyboard_gestures", 0));
    attach_full(kt, 7, g_gestures_chk);
    g_signal_connect(g_sticky_chk, "toggled", G_CALLBACK(sync_sensitive), g_sticky_two_chk);
    g_signal_connect(g_slow_chk, "toggled", G_CALLBACK(sync_sensitive), g_slow_spin);
    g_signal_connect(g_bounce_chk, "toggled", G_CALLBACK(sync_sensitive), g_bounce_spin);
    sync_sensitive(g_sticky_chk, g_sticky_two_chk);
    sync_sensitive(g_slow_chk, g_slow_spin);
    sync_sensitive(g_bounce_chk, g_bounce_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with(_("Teclado"), kt), FALSE, FALSE, 0);

    /* Vision */
    GtkWidget *vt = gtk_table_new(3, 2, FALSE);
    g_scale_spin = gtk_spin_button_new_with_range(0.5, 3.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_scale_spin), 2);
    const char *scale = kv_find(&a11y, "text_scale");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_scale_spin), scale ? g_ascii_strtod(scale, NULL) : 1.0);
    labeled_row(vt, 0, _("Escala do texto:"), g_scale_spin);
    g_cursor_spin = gtk_spin_button_new_with_range(16, 128, 8);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_spin), kv_int(&conf, "cursor_size", 24));
    labeled_row(vt, 1, _("Tamanho do cursor (px):"), g_cursor_spin);
    g_hc_chk = check(_("Alto contraste (texto branco sobre preto, selecao amarela)"), kv_int(&a11y, "high_contrast", 0));
    attach_full(vt, 2, g_hc_chk);
    gtk_box_pack_start(GTK_BOX(outer), frame_with(_("Visao"), vt), FALSE, FALSE, 0);

    /* Assistive technology */
    GtkWidget *at = gtk_table_new(3, 2, FALSE);
    int ia = kisession_index("a11y"), io = kisession_index("screenreader");
    g_atspi_chk = check(_("Suporte a tecnologias assistivas (AT-SPI)"), ia >= 0 && ks.enabled[ia]);
    attach_full(at, 0, g_atspi_chk);
    g_orca_chk = check(_("Leitor de tela (orca)"), io >= 0 && ks.enabled[io]);
    attach_full(at, 1, g_orca_chk);
    GtkWidget *note = gtk_label_new(_("Vale a partir do proximo login. Ctrl+Alt+Tab foca o painel para usa-lo pelo "
                                      "teclado (focus_key= no xispanel.conf)."));
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    attach_full(at, 2, note);
    gtk_box_pack_start(GTK_BOX(outer), frame_with(_("Tecnologias assistivas"), at), FALSE, FALSE, 0);

    GtkWidget *apply = gtk_button_new_with_label(_("Aplicar"));
    g_signal_connect(apply, "clicked", G_CALLBACK(apply_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);
    return scroll;
}
