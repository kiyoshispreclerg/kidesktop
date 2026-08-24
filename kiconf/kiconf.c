/*
 * kiconf - GTK2 configurator for KiDesktop, remake of xisconf (Python/Qt).
 *
 * Prototype scope: two working tabs plus placeholders for the rest of
 * xisconf's feature set (Screens, Pointer/Keyboard, Permissions), which
 * already has a known implementation path -- xrandr/xinput/xset
 * subprocess calls and the xisguard control socket, same as xisconf.py --
 * just not ported to GTK2/C yet. What's new here and actually implemented:
 *
 *   Aparencia -- edits kiconfd's cursor theme/size, color palette, GTK2/3/4
 *     + icon theme names, Qt style, and general/monospace fonts, all
 *     directly in $XDG_CONFIG_HOME/kiconfd.conf (fallback
 *     ~/.config/kiconfd.conf), then signals the running kiconfd with
 *     SIGHUP to reload+reapply. Theme/icon/cursor pickers are comboboxes
 *     populated by actually scanning what's installed (/usr/share/themes,
 *     /usr/share/icons, ~/.themes, ~/.icons) rather than free text, so the
 *     user can only pick something that exists; the Qt style list is a
 *     static fallback (Fusion/Windows/gtk2) since enumerating installed
 *     QStyle plugins would need linking against Qt itself.
 *   Atalhos -- edits xiskeys' BIND lines directly in
 *     $XDG_CONFIG_HOME/xiskeys.conf (fallback ~/.config/xiskeys.conf),
 *     then signals the running xiskeys with SIGHUP to reload.
 *
 * Neither daemon has a control socket yet (see XISDESKTOP_PLAN.md), so
 * kiconf edits their config files directly and reloads via SIGHUP --
 * exactly the file format each daemon itself reads/writes, nothing new
 * invented here. Moving to a socket protocol later is a daemon-side
 * change; kiconf's tabs wouldn't need to change shape, just how they
 * push/pull data.
 */
#include <gtk/gtk.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME_LEN 128
#define MAX_THEMES 128

enum { COL_ACTION = 0, COL_SPEC, COL_COMMAND, N_SHORTCUT_COLS };

static GtkListStore *g_shortcuts_store;

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

/* ---- config path resolution (mirrors kiconfd.c/xiskeys.c exactly) ---- */

static void resolve_path(const char *filename, char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/%s", xdg_config, filename);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(out, outsz, "%s/.config/%s", home, filename);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return s;
}

/* Sends SIGHUP to every process named `procname` (exact match, not -f --
 * safe against accidentally hitting unrelated processes). Fire-and-forget,
 * same spirit as run_action() elsewhere in this repo's daemons. */
static void signal_daemon(const char *procname)
{
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execlp("pkill", "pkill", "-HUP", "-x", procname, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
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

static GtkWidget *labeled_row(GtkWidget *table, int row, const char *label_text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1, GTK_FILL, GTK_FILL, 4, 3);
    gtk_table_attach(GTK_TABLE(table), widget, 1, 2, row, row + 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 4, 3);
    return widget;
}

static GtkWidget *frame_with(const char *title, GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *build_appearance_tab(void)
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

    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(save_appearance_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}

/* ---- Atalhos tab: xiskeys.conf (BIND action spec command) ------------ */

static void load_shortcuts(void)
{
    gtk_list_store_clear(g_shortcuts_store);

    char path[PATH_MAX];
    resolve_path("xiskeys.conf", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *save = NULL;
        char *tag = strtok_r(line, "\t", &save);
        char *action = tag ? strtok_r(NULL, "\t", &save) : NULL;
        char *spec = action ? strtok_r(NULL, "\t", &save) : NULL;
        char *cmd = spec ? strtok_r(NULL, "", &save) : NULL;
        if (!tag || strcmp(tag, "BIND") != 0 || !action || !spec || !cmd) {
            continue;
        }
        GtkTreeIter it;
        gtk_list_store_append(g_shortcuts_store, &it);
        gtk_list_store_set(g_shortcuts_store, &it,
                            COL_ACTION, action, COL_SPEC, spec, COL_COMMAND, cmd, -1);
    }
    fclose(f);
}

static void save_shortcuts_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char path[PATH_MAX];
    resolve_path("xiskeys.conf", path, sizeof(path));

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# xiskeys config -- one binding per line:\n");
    fprintf(f, "#   BIND\\t<action-name>\\t<Mod>+<Mod>+<Key>\\t<shell command>\n");

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_shortcuts_store), &it);
    while (valid) {
        gchar *action, *spec, *cmd;
        gtk_tree_model_get(GTK_TREE_MODEL(g_shortcuts_store), &it,
                            COL_ACTION, &action, COL_SPEC, &spec, COL_COMMAND, &cmd, -1);
        if (action && spec && cmd && *action && *spec && *cmd) {
            fprintf(f, "BIND\t%s\t%s\t%s\n", action, spec, cmd);
        }
        g_free(action);
        g_free(spec);
        g_free(cmd);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_shortcuts_store), &it);
    }
    fclose(f);
    rename(tmp, path);

    signal_daemon("xiskeys");
}

static void add_shortcut_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_shortcuts_store, &it);
    gtk_list_store_set(g_shortcuts_store, &it,
                        COL_ACTION, "novo-atalho", COL_SPEC, "Ctrl+F1", COL_COMMAND, "true", -1);
}

static void remove_shortcut_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_shortcuts_store, &it);
    }
}

static void shortcut_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_shortcuts_store), &it, path)) {
        gtk_list_store_set(g_shortcuts_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *build_shortcuts_tab(void)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    g_shortcuts_store = gtk_list_store_new(N_SHORTCUT_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_shortcuts_store));

    const char *titles[N_SHORTCUT_COLS] = {"Acao", "Atalho", "Comando"};
    for (int col = 0; col < N_SHORTCUT_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, NULL);
        g_signal_connect(renderer, "edited", G_CALLBACK(shortcut_cell_edited), GINT_TO_POINTER(col));
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), tvcol);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label("Adicionar");
    GtkWidget *remove_btn = gtk_button_new_with_label("Remover");
    GtkWidget *save_btn = gtk_button_new_with_label("Salvar e recarregar xiskeys");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(add_shortcut_cb), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(remove_shortcut_cb), view);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_shortcuts_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

    load_shortcuts();
    return vbox;
}

/* ---- placeholder tabs for the rest of xisconf's feature set ---------- */

static GtkWidget *build_placeholder_tab(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(label), 0.5, 0.3);
    gtk_container_set_border_width(GTK_CONTAINER(label), 12);
    return label;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "kiconf");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 420);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_appearance_tab(), gtk_label_new("Aparencia"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_shortcuts_tab(), gtk_label_new("Atalhos"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                              build_placeholder_tab("TODO: portar de xisconf.py\n(xrandr / DPI por saida)"),
                              gtk_label_new("Telas"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                              build_placeholder_tab("TODO: portar de xisconf.py\n(xinput / xset / flags XiS)"),
                              gtk_label_new("Entrada"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                              build_placeholder_tab("TODO: portar de xisconf.py\n(socket de controle do xisguard)"),
                              gtk_label_new("Permissoes"));

    gtk_container_add(GTK_CONTAINER(window), notebook);
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
