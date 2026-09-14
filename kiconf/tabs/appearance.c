/* kiconf - Aparencia tab: kiconfd.conf (cursor/theme/color/font settings).
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Aparencia tab widgets */
static GtkWidget *g_cursor_combo;
static GtkWidget *g_cursor_size_spin;
static GtkWidget *g_gtk2_combo;
static GtkWidget *g_gtk3_combo;
static GtkWidget *g_gtk4_combo;
static GtkWidget *g_icon_combo;
static GtkWidget *g_qt_style_combo;
static GtkWidget *g_color_bg_btn;
static GtkWidget *g_color_fg_btn;
static GtkWidget *g_color_base_btn;
static GtkWidget *g_color_accent_btn;
static GtkWidget *g_color_selbg_btn;
static GtkWidget *g_color_selfg_btn;
static GtkWidget *g_font_general_btn;
static GtkWidget *g_font_mono_btn;

/* ---- installed-theme scanning ----------------------------------------- */

#define MAX_THEMES 128

typedef struct {
    char names[MAX_THEMES][NAME_LEN];
    int n;
} ThemeList;

static int theme_list_has(ThemeList *tl, const char *name)
{
    for (int i = 0; i < tl->n; i++) {
        if (!strcmp(tl->names[i], name)) {
            return 1;
        }
    }
    return 0;
}

static void theme_list_add(ThemeList *tl, const char *name)
{
    if (tl->n >= MAX_THEMES || theme_list_has(tl, name)) {
        return;
    }
    snprintf(tl->names[tl->n], NAME_LEN, "%s", name);
    tl->n++;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void theme_list_sort(ThemeList *tl)
{
    qsort(tl->names, (size_t)tl->n, NAME_LEN, cmp_names);
}

/* Scans `bases` for subdirectories that themselves contain `marker`
 * (a subdirectory when `marker_is_dir` else a plain file) -- this is how
 * GTK theme dirs (marker="gtk-3.0"), icon themes (marker="index.theme",
 * file) and cursor themes (marker="cursors", dir) are all detected: a
 * theme is "installed" iff that marker exists under it. */
static void scan_marker_dirs(ThemeList *tl, const char *const *bases, const char *marker, int marker_is_dir)
{
    for (int b = 0; bases[b]; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) {
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') {
                continue;
            }
            char check[PATH_MAX];
            snprintf(check, sizeof(check), "%s/%s/%s", bases[b], e->d_name, marker);
            struct stat st;
            if (stat(check, &st) == 0 && (!marker_is_dir || S_ISDIR(st.st_mode))) {
                theme_list_add(tl, e->d_name);
            }
        }
        closedir(d);
    }
}

static void themes_dirs(char *home_themes, size_t sz, const char **bases)
{
    const char *home = getenv("HOME");
    snprintf(home_themes, sz, "%s/.themes", home ? home : "");
    bases[0] = "/usr/share/themes";
    bases[1] = home_themes;
    bases[2] = NULL;
}

static void icons_dirs(char *home_icons, size_t sz, const char **bases)
{
    const char *home = getenv("HOME");
    snprintf(home_icons, sz, "%s/.icons", home ? home : "");
    bases[0] = "/usr/share/icons";
    bases[1] = home_icons;
    bases[2] = NULL;
}

/* GTK2/3/4 theme dirs live under the *same* $theme/ directory, each
 * version's assets in its own gtk-X.0 subdirectory -- so a theme that
 * only ships a gtk-2.0 folder simply won't show up in the GTK3/4
 * comboboxes, which is the correct/expected behavior. */
static ThemeList scan_gtk_themes(const char *gtk_subdir)
{
    ThemeList tl = {.n = 0};
    char home_themes[PATH_MAX];
    const char *bases[3];
    themes_dirs(home_themes, sizeof(home_themes), bases);
    scan_marker_dirs(&tl, bases, gtk_subdir, 1);
    return tl;
}

static ThemeList scan_icon_themes(void)
{
    ThemeList tl = {.n = 0};
    char home_icons[PATH_MAX];
    const char *bases[3];
    icons_dirs(home_icons, sizeof(home_icons), bases);
    scan_marker_dirs(&tl, bases, "index.theme", 0);
    return tl;
}

static ThemeList scan_cursor_themes(void)
{
    ThemeList tl = {.n = 0};
    char home_icons[PATH_MAX];
    const char *bases[3];
    icons_dirs(home_icons, sizeof(home_icons), bases);
    scan_marker_dirs(&tl, bases, "cursors", 1);
    return tl;
}

/* Builds a GTK2 text combobox from `tl`, pre-selecting `current` -- if
 * `current` isn't among the scanned entries (a config value that predates
 * a theme being uninstalled, or just untested) it's appended too, so the
 * combo never silently discards what's actually configured. */
static GtkWidget *make_theme_combo(ThemeList *tl, const char *current)
{
    theme_list_sort(tl);
    GtkWidget *combo = gtk_combo_box_new_text();
    int idx = -1;
    for (int i = 0; i < tl->n; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), tl->names[i]);
        if (!strcmp(tl->names[i], current)) {
            idx = i;
        }
    }
    if (idx < 0 && current && current[0]) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), current);
        idx = tl->n;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx >= 0 ? idx : (tl->n > 0 ? 0 : -1));
    return combo;
}

static gchar *combo_active_text_or(GtkWidget *combo, const char *fallback)
{
    gchar *t = gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo));
    if (t && *t) {
        return t;
    }
    g_free(t);
    return g_strdup(fallback);
}

/* ---- Aparencia tab: kiconfd.conf (theme/color/font settings) --------- */

typedef struct {
    char cursor_theme[NAME_LEN];
    int cursor_size;
    char color_bg[16], color_fg[16], color_base[16];
    char color_accent[16], color_selection_bg[16], color_selection_fg[16];
    char font_general[NAME_LEN], font_monospace[NAME_LEN];
    char gtk2_theme[NAME_LEN], gtk3_theme[NAME_LEN], gtk4_theme[NAME_LEN];
    char icon_theme[NAME_LEN], qt_style[NAME_LEN];
} Appearance;

/* Defaults mirror kiconfd's own apply_and_persist_defaults() -- if
 * kiconfd.conf doesn't exist yet (kiconfd never ran), the UI should still
 * show something sane instead of blank widgets. */
static void appearance_defaults(Appearance *a)
{
    snprintf(a->cursor_theme, sizeof(a->cursor_theme), "default");
    a->cursor_size = 24;
    snprintf(a->color_bg, sizeof(a->color_bg), "#e0e0e0");
    snprintf(a->color_fg, sizeof(a->color_fg), "#202020");
    snprintf(a->color_base, sizeof(a->color_base), "#ffffff");
    snprintf(a->color_accent, sizeof(a->color_accent), "#3584e4");
    snprintf(a->color_selection_bg, sizeof(a->color_selection_bg), "#3584e4");
    snprintf(a->color_selection_fg, sizeof(a->color_selection_fg), "#ffffff");
    snprintf(a->font_general, sizeof(a->font_general), "Sans 10");
    snprintf(a->font_monospace, sizeof(a->font_monospace), "Monospace 10");
    snprintf(a->gtk2_theme, sizeof(a->gtk2_theme), "Default");
    snprintf(a->gtk3_theme, sizeof(a->gtk3_theme), "Adwaita");
    snprintf(a->gtk4_theme, sizeof(a->gtk4_theme), "Adwaita");
    snprintf(a->icon_theme, sizeof(a->icon_theme), "Adwaita");
    snprintf(a->qt_style, sizeof(a->qt_style), "Fusion");
}

static void load_appearance(Appearance *a)
{
    appearance_defaults(a);

    char path[PATH_MAX];
    resolve_path("kiconfd.conf", path, sizeof(path));
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
#define SET(k, field) if (!strcmp(key, k)) snprintf(a->field, sizeof(a->field), "%s", val)
        SET("cursor_theme", cursor_theme);
        else if (!strcmp(key, "cursor_size")) a->cursor_size = atoi(val);
        else SET("color_bg", color_bg);
        else SET("color_fg", color_fg);
        else SET("color_base", color_base);
        else SET("color_accent", color_accent);
        else SET("color_selection_bg", color_selection_bg);
        else SET("color_selection_fg", color_selection_fg);
        else SET("font_general", font_general);
        else SET("font_monospace", font_monospace);
        else SET("gtk2_theme", gtk2_theme);
        else SET("gtk3_theme", gtk3_theme);
        else SET("gtk4_theme", gtk4_theme);
        else SET("icon_theme", icon_theme);
        else SET("qt_style", qt_style);
#undef SET
    }
    fclose(f);
}

static GtkWidget *make_color_button(const char *hex)
{
    GdkColor c;
    if (!gdk_color_parse(hex, &c)) {
        gdk_color_parse("#000000", &c);
    }
    return gtk_color_button_new_with_color(&c);
}

static void color_button_hex(GtkWidget *btn, char *out, size_t outsz)
{
    GdkColor c;
    gtk_color_button_get_color(GTK_COLOR_BUTTON(btn), &c);
    snprintf(out, outsz, "#%02x%02x%02x", c.red >> 8, c.green >> 8, c.blue >> 8);
}

static void save_appearance_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    Appearance a;
    gchar *cursor = combo_active_text_or(g_cursor_combo, "default");
    snprintf(a.cursor_theme, sizeof(a.cursor_theme), "%s", cursor);
    g_free(cursor);
    a.cursor_size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_cursor_size_spin));

    color_button_hex(g_color_bg_btn, a.color_bg, sizeof(a.color_bg));
    color_button_hex(g_color_fg_btn, a.color_fg, sizeof(a.color_fg));
    color_button_hex(g_color_base_btn, a.color_base, sizeof(a.color_base));
    color_button_hex(g_color_accent_btn, a.color_accent, sizeof(a.color_accent));
    color_button_hex(g_color_selbg_btn, a.color_selection_bg, sizeof(a.color_selection_bg));
    color_button_hex(g_color_selfg_btn, a.color_selection_fg, sizeof(a.color_selection_fg));

    const gchar *fg = gtk_font_button_get_font_name(GTK_FONT_BUTTON(g_font_general_btn));
    snprintf(a.font_general, sizeof(a.font_general), "%s", fg ? fg : "Sans 10");
    const gchar *fm = gtk_font_button_get_font_name(GTK_FONT_BUTTON(g_font_mono_btn));
    snprintf(a.font_monospace, sizeof(a.font_monospace), "%s", fm ? fm : "Monospace 10");

    gchar *gtk2 = combo_active_text_or(g_gtk2_combo, "Default");
    snprintf(a.gtk2_theme, sizeof(a.gtk2_theme), "%s", gtk2);
    g_free(gtk2);
    gchar *gtk3 = combo_active_text_or(g_gtk3_combo, "Adwaita");
    snprintf(a.gtk3_theme, sizeof(a.gtk3_theme), "%s", gtk3);
    g_free(gtk3);
    gchar *gtk4 = combo_active_text_or(g_gtk4_combo, "Adwaita");
    snprintf(a.gtk4_theme, sizeof(a.gtk4_theme), "%s", gtk4);
    g_free(gtk4);
    gchar *icon = combo_active_text_or(g_icon_combo, "Adwaita");
    snprintf(a.icon_theme, sizeof(a.icon_theme), "%s", icon);
    g_free(icon);
    gchar *qtstyle = combo_active_text_or(g_qt_style_combo, "Fusion");
    snprintf(a.qt_style, sizeof(a.qt_style), "%s", qtstyle);
    g_free(qtstyle);

    char path[PATH_MAX];
    resolve_path("kiconfd.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# kiconfd config\n");
    fprintf(f, "cursor_theme = %s\n", a.cursor_theme);
    fprintf(f, "cursor_size = %d\n", a.cursor_size);
    fprintf(f, "color_bg = %s\n", a.color_bg);
    fprintf(f, "color_fg = %s\n", a.color_fg);
    fprintf(f, "color_base = %s\n", a.color_base);
    fprintf(f, "color_accent = %s\n", a.color_accent);
    fprintf(f, "color_selection_bg = %s\n", a.color_selection_bg);
    fprintf(f, "color_selection_fg = %s\n", a.color_selection_fg);
    fprintf(f, "font_general = %s\n", a.font_general);
    fprintf(f, "font_monospace = %s\n", a.font_monospace);
    fprintf(f, "gtk2_theme = %s\n", a.gtk2_theme);
    fprintf(f, "gtk3_theme = %s\n", a.gtk3_theme);
    fprintf(f, "gtk4_theme = %s\n", a.gtk4_theme);
    fprintf(f, "icon_theme = %s\n", a.icon_theme);
    fprintf(f, "qt_style = %s\n", a.qt_style);
    fclose(f);
    rename(tmp, path);

    signal_daemon("kiconfd");
}

/* ---- Aparencia tab: "Importar da sessao atual" ------------------------
 * kiconfd.conf (read by load_appearance() above) only reflects what
 * kiconfd itself last applied -- if kiconfd never ran yet in this session,
 * or the user set a cursor/theme by hand (lxappearance, qt5ct directly,
 * a distro default), the tab shows stale/default values instead of
 * what's actually active. This scans the same live places kiconfd itself
 * writes to when it *does* run (so "Importar" then "Aplicar" is a no-op),
 * plus the X resource database as a cursor fallback, and only fills the
 * widgets -- nothing touches disk until "Aplicar" is clicked. The color
 * palette has no independent live source (it's a kiconfd-only construct,
 * no toolkit exposes "the current accent color" generically), so it's
 * left untouched by this. Monospace font is left alone too: unlike the
 * general font (GTK3's settings.ini gtk-font-name), there's no single
 * cross-toolkit place a "monospace font" is recorded system-wide. */

static int read_ini_value(const char *path, const char *section, const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        return 0;
    }
    gchar *v = g_key_file_get_string(kf, section, key, NULL);
    int ok = 0;
    if (v && *v) {
        snprintf(out, outsz, "%s", v);
        ok = 1;
    }
    g_free(v);
    g_key_file_free(kf);
    return ok;
}

/* ~/.gtkrc-2.0 isn't ini-format (no [section]s), just "key = value" lines
 * that GTK2 itself parses top-to-bottom with later lines winning -- scans
 * the whole file and keeps the last match, so kiconf's own "# BEGIN/END
 * KICONF" block (appended at the end) correctly takes priority when
 * present, same as it does for GTK2 itself. */
static int read_gtkrc2_value(const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.gtkrc-2.0", home ? home : "");
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char line[512];
    int found = 0;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (strncmp(l, key, klen) != 0) {
            continue;
        }
        char *rest = trim(l + klen);
        if (*rest != '=') {
            continue;
        }
        rest = trim(rest + 1);
        size_t len = strlen(rest);
        if (len >= 2 && rest[0] == '"' && rest[len - 1] == '"') {
            rest[len - 1] = '\0';
            rest++;
        }
        snprintf(out, outsz, "%s", rest);
        found = 1;
    }
    fclose(f);
    return found;
}

/* Grabs the value following "propname:" up to end-of-line out of `xrdb
 * -query`'s output -- same tab-after-colon format Xresources always use. */
static int read_xrdb_prop(const char *xrdb_out, const char *propname, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *p = strstr(xrdb_out, propname);
    if (!p) {
        return 0;
    }
    p += strlen(propname);
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    char tmp[256];
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    char *t = trim(tmp);
    if (!*t) {
        return 0;
    }
    snprintf(out, outsz, "%s", t);
    return 1;
}

/* Selects `name` in a text combobox built by make_theme_combo(), appending
 * it (like make_theme_combo() itself does for the on-disk current value)
 * if the live value isn't one of the scanned/installed choices. */
static void combo_select_or_append(GtkWidget *combo, const char *name)
{
    if (!name || !*name) {
        return;
    }
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter it;
    int idx = 0, found = -1;
    if (gtk_tree_model_get_iter_first(model, &it)) {
        do {
            gchar *t = NULL;
            gtk_tree_model_get(model, &it, 0, &t, -1);
            if (t && !strcmp(t, name)) {
                found = idx;
            }
            g_free(t);
            idx++;
        } while (found < 0 && gtk_tree_model_iter_next(model, &it));
    }
    if (found < 0) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), name);
        found = idx;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), found);
}

static void import_appearance_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char val[NAME_LEN];
    char path[PATH_MAX];

    /* Cursor theme/size: the X resource database is what X clients that
     * don't read GTK/Qt settings actually use, so it's the most "session
     * truth" source available; GTK3's settings.ini is the fallback. */
    char xrdb_out[8192];
    char *xrdb_argv[] = {"xrdb", "-query", NULL};
    int have_xrdb = run_capture(xrdb_argv, xrdb_out, sizeof(xrdb_out));
    int got_cursor_theme = 0, got_cursor_size = 0;
    if (have_xrdb && read_xrdb_prop(xrdb_out, "Xcursor.theme:", val, sizeof(val))) {
        combo_select_or_append(g_cursor_combo, val);
        got_cursor_theme = 1;
    }
    if (have_xrdb && read_xrdb_prop(xrdb_out, "Xcursor.size:", val, sizeof(val)) && atoi(val) > 0) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_size_spin), atoi(val));
        got_cursor_size = 1;
    }

    resolve_path("gtk-3.0/settings.ini", path, sizeof(path));
    if (read_ini_value(path, "Settings", "gtk-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_gtk3_combo, val);
    }
    if (read_ini_value(path, "Settings", "gtk-icon-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_icon_combo, val);
    }
    if (read_ini_value(path, "Settings", "gtk-font-name", val, sizeof(val))) {
        gtk_font_button_set_font_name(GTK_FONT_BUTTON(g_font_general_btn), val);
    }
    if (!got_cursor_theme && read_ini_value(path, "Settings", "gtk-cursor-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_cursor_combo, val);
    }
    if (!got_cursor_size && read_ini_value(path, "Settings", "gtk-cursor-theme-size", val, sizeof(val)) && atoi(val) > 0) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_size_spin), atoi(val));
    }

    resolve_path("gtk-4.0/settings.ini", path, sizeof(path));
    if (read_ini_value(path, "Settings", "gtk-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_gtk4_combo, val);
    }

    if (read_gtkrc2_value("gtk-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_gtk2_combo, val);
    }
    if (read_gtkrc2_value("gtk-icon-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_icon_combo, val);
    }

    resolve_path("qt5ct/qt5ct.conf", path, sizeof(path));
    if (!read_ini_value(path, "Appearance", "style", val, sizeof(val))) {
        resolve_path("qt6ct/qt6ct.conf", path, sizeof(path));
        read_ini_value(path, "Appearance", "style", val, sizeof(val));
    }
    if (val[0]) {
        combo_select_or_append(g_qt_style_combo, val);
    }
}


GtkWidget *build_appearance_tab(void)
{
    Appearance a;
    load_appearance(&a);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    /* Cursor */
    GtkWidget *cursor_table = gtk_table_new(2, 2, FALSE);
    ThemeList cursors = scan_cursor_themes();
    g_cursor_combo = make_theme_combo(&cursors, a.cursor_theme);
    labeled_row(cursor_table, 0, "Tema de cursor:", g_cursor_combo);
    g_cursor_size_spin = gtk_spin_button_new_with_range(8, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_size_spin), a.cursor_size > 0 ? a.cursor_size : 24);
    labeled_row(cursor_table, 1, "Tamanho:", g_cursor_size_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Cursor", cursor_table), FALSE, FALSE, 0);

    /* Toolkit themes */
    GtkWidget *themes_table = gtk_table_new(5, 2, FALSE);
    ThemeList gtk2t = scan_gtk_themes("gtk-2.0");
    g_gtk2_combo = make_theme_combo(&gtk2t, a.gtk2_theme);
    labeled_row(themes_table, 0, "Tema GTK2:", g_gtk2_combo);
    ThemeList gtk3t = scan_gtk_themes("gtk-3.0");
    g_gtk3_combo = make_theme_combo(&gtk3t, a.gtk3_theme);
    labeled_row(themes_table, 1, "Tema GTK3:", g_gtk3_combo);
    ThemeList gtk4t = scan_gtk_themes("gtk-4.0");
    g_gtk4_combo = make_theme_combo(&gtk4t, a.gtk4_theme);
    labeled_row(themes_table, 2, "Tema GTK4:", g_gtk4_combo);
    ThemeList icons = scan_icon_themes();
    g_icon_combo = make_theme_combo(&icons, a.icon_theme);
    labeled_row(themes_table, 3, "Tema de icones:", g_icon_combo);
    g_qt_style_combo = gtk_combo_box_new_text();
    {
        static const char *qt_styles[] = {"Fusion", "Windows", "gtk2", NULL};
        int idx = -1;
        for (int i = 0; qt_styles[i]; i++) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_qt_style_combo), qt_styles[i]);
            if (!strcmp(qt_styles[i], a.qt_style)) {
                idx = i;
            }
        }
        if (idx < 0) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_qt_style_combo), a.qt_style);
            idx = 3;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_qt_style_combo), idx);
    }
    labeled_row(themes_table, 4, "Estilo Qt (aprox.):", g_qt_style_combo);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Temas dos toolkits", themes_table), FALSE, FALSE, 0);

    /* Color palette */
    GtkWidget *colors_table = gtk_table_new(6, 2, FALSE);
    g_color_bg_btn = labeled_row(colors_table, 0, "Fundo:", make_color_button(a.color_bg));
    g_color_fg_btn = labeled_row(colors_table, 1, "Texto:", make_color_button(a.color_fg));
    g_color_base_btn = labeled_row(colors_table, 2, "Fundo de campos:", make_color_button(a.color_base));
    g_color_accent_btn = labeled_row(colors_table, 3, "Destaque (accent):", make_color_button(a.color_accent));
    g_color_selbg_btn = labeled_row(colors_table, 4, "Selecao (fundo):", make_color_button(a.color_selection_bg));
    g_color_selfg_btn = labeled_row(colors_table, 5, "Selecao (texto):", make_color_button(a.color_selection_fg));
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Paleta de cores", colors_table), FALSE, FALSE, 0);

    /* Fonts */
    GtkWidget *fonts_table = gtk_table_new(2, 2, FALSE);
    g_font_general_btn = gtk_font_button_new_with_font(a.font_general);
    labeled_row(fonts_table, 0, "Fonte geral:", g_font_general_btn);
    g_font_mono_btn = gtk_font_button_new_with_font(a.font_monospace);
    labeled_row(fonts_table, 1, "Fonte monoespacada:", g_font_mono_btn);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Fontes", fonts_table), FALSE, FALSE, 0);

    GtkWidget *import_btn = gtk_button_new_with_label("Importar da sessao atual");
    g_signal_connect(import_btn, "clicked", G_CALLBACK(import_appearance_cb), NULL);
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(save_appearance_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(btnbox), import_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}
