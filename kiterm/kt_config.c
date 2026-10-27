/* kt_config.c - flat key=value file, $XDG_CONFIG_HOME/kiterm.conf (fallback
 * ~/.config/kiterm.conf), same style as kiwm.conf/xispanel.conf. Unknown keys
 * and bad values are ignored so a hand-edited file never stops the terminal
 * from opening. Decimals are parsed with g_ascii_strtod(): GTK resets
 * LC_NUMERIC on startup and pt_BR would otherwise read "0.85" as 0. */
#include "kt_config.h"
#include "../shared/xis_i18n.h"
#include <vte/vte.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "../shared/xis_safefile.h"

const KtScheme kt_schemes[] = {
    { "default", N_("Dark"), "#d3d7cf", "#101114", "#d3d7cf",
      { "#2e3436", "#cc0000", "#4e9a06", "#c4a000", "#3465a4", "#75507b", "#06989a", "#d3d7cf",
        "#555753", "#ef2929", "#8ae234", "#fce94f", "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec" } },
    { "light", N_("Light"), "#2e3436", "#fafafa", "#2e3436",
      { "#2e3436", "#cc0000", "#4e9a06", "#c4a000", "#3465a4", "#75507b", "#06989a", "#d3d7cf",
        "#555753", "#ef2929", "#8ae234", "#fce94f", "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec" } },
    { "solarized-dark", N_("Solarized dark"), "#839496", "#002b36", "#93a1a1",
      { "#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
        "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3" } },
    { "solarized-light", N_("Solarized light"), "#657b83", "#fdf6e3", "#586e75",
      { "#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
        "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3" } },
    { "linux", N_("Linux console"), "#aaaaaa", "#000000", "#aaaaaa",
      { "#000000", "#aa0000", "#00aa00", "#aa5500", "#0000aa", "#aa00aa", "#00aaaa", "#aaaaaa",
        "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff" } },
    { "green", N_("Green on black"), "#33ff33", "#000000", "#33ff33",
      { "#000000", "#aa0000", "#00aa00", "#aa5500", "#0000aa", "#aa00aa", "#00aaaa", "#aaaaaa",
        "#555555", "#ff5555", "#55ff55", "#ffff55", "#5555ff", "#ff55ff", "#55ffff", "#ffffff" } },
};
const int kt_n_schemes = sizeof(kt_schemes) / sizeof(kt_schemes[0]);

void kt_config_apply_scheme(KtConfig *c, const KtScheme *s)
{
    gdk_color_parse(s->fg, &c->fg);
    gdk_color_parse(s->bg, &c->bg);
    gdk_color_parse(s->cursor, &c->cursor);
    for (int i = 0; i < 16; i++)
        gdk_color_parse(s->palette[i], &c->palette[i]);
}

void kt_config_defaults(KtConfig *c)
{
    memset(c, 0, sizeof(*c));
    g_strlcpy(c->font, "Monospace 11", sizeof(c->font));
    c->scrollback = 10000;
    c->opacity = 1.0;
    c->cursor_shape = VTE_CURSOR_SHAPE_BLOCK;
    c->cursor_blink = TRUE;
    c->audible_bell = TRUE;      /* plays the sound theme's "bell"; the theme decides if it is heard */
    c->notify_done = TRUE;
    c->notify_done_secs = 10;
    c->urgent_on_bell = TRUE;
    c->scroll_on_output = FALSE;
    c->scroll_on_keystroke = TRUE;
    c->confirm_close = TRUE;
    c->tabs_pos = KT_TABS_TOP;
    g_strlcpy(c->word_chars, "-A-Za-z0-9,./?%&#:_=+@~", sizeof(c->word_chars));
    c->cols = 100;
    c->rows = 30;
    kt_config_apply_scheme(c, &kt_schemes[0]);
}

static char *config_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kiterm.conf", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "kiterm.conf", NULL);
}

static gboolean parse_bool(const char *v)
{
    return !g_ascii_strcasecmp(v, "1") || !g_ascii_strcasecmp(v, "true") ||
           !g_ascii_strcasecmp(v, "yes") || !g_ascii_strcasecmp(v, "on");
}

static const char *const kShapes[] = { "block", "ibeam", "underline" };
static const char *const kTabsPos[] = { "top", "bottom", "left", "right" };

static int index_of(const char *const *tab, int n, const char *v, int fallback)
{
    for (int i = 0; i < n; i++)
        if (!g_ascii_strcasecmp(tab[i], v))
            return i;
    return fallback;
}

static void set_color(GdkColor *dst, const char *v)
{
    GdkColor tmp;
    if (gdk_color_parse(v, &tmp))
        *dst = tmp;
}

void kt_config_load(KtConfig *c)
{
    kt_config_defaults(c);
    char *path = config_path();
    FILE *f = xis_fopen_read(path, xis_check_usable);
    g_free(path);
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = g_strstrip(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = g_strstrip(s), *v = g_strstrip(eq + 1);
        if (!strcmp(k, "font")) g_strlcpy(c->font, v, sizeof(c->font));
        else if (!strcmp(k, "scheme")) {
            for (int i = 0; i < kt_n_schemes; i++)
                if (!strcmp(kt_schemes[i].id, v))
                    kt_config_apply_scheme(c, &kt_schemes[i]);
        }
        else if (!strcmp(k, "scrollback")) c->scrollback = CLAMP(atoi(v), 0, 1000000);
        else if (!strcmp(k, "opacity")) c->opacity = CLAMP(g_ascii_strtod(v, NULL), 0.1, 1.0);
        else if (!strcmp(k, "foreground")) set_color(&c->fg, v);
        else if (!strcmp(k, "background")) set_color(&c->bg, v);
        else if (!strcmp(k, "cursor_color")) set_color(&c->cursor, v);
        else if (!strncmp(k, "color", 5) && atoi(k + 5) >= 0 && atoi(k + 5) < 16 && isdigit((unsigned char)k[5]))
            set_color(&c->palette[atoi(k + 5)], v);
        else if (!strcmp(k, "cursor_shape")) c->cursor_shape = index_of(kShapes, 3, v, 0);
        else if (!strcmp(k, "cursor_blink")) c->cursor_blink = parse_bool(v);
        else if (!strcmp(k, "audible_bell")) c->audible_bell = parse_bool(v);
        else if (!strcmp(k, "notify_done")) c->notify_done = parse_bool(v);
        else if (!strcmp(k, "notify_done_secs")) c->notify_done_secs = CLAMP(atoi(v), 1, 3600);
        else if (!strcmp(k, "urgent_on_bell")) c->urgent_on_bell = parse_bool(v);
        else if (!strcmp(k, "scroll_on_output")) c->scroll_on_output = parse_bool(v);
        else if (!strcmp(k, "scroll_on_keystroke")) c->scroll_on_keystroke = parse_bool(v);
        else if (!strcmp(k, "confirm_close")) c->confirm_close = parse_bool(v);
        else if (!strcmp(k, "tabs")) c->tabs_pos = index_of(kTabsPos, 4, v, 0);
        else if (!strcmp(k, "shell")) g_strlcpy(c->shell, v, sizeof(c->shell));
        else if (!strcmp(k, "word_chars")) g_strlcpy(c->word_chars, v, sizeof(c->word_chars));
        else if (!strcmp(k, "columns")) c->cols = CLAMP(atoi(v), 20, 500);
        else if (!strcmp(k, "rows")) c->rows = CLAMP(atoi(v), 5, 200);
    }
    fclose(f);
}

static void put_color(FILE *f, const char *key, const GdkColor *c)
{
    fprintf(f, "%s=#%02x%02x%02x\n", key, c->red >> 8, c->green >> 8, c->blue >> 8);
}

gboolean kt_config_save(const KtConfig *c)
{
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    char tmp[4096];
    xis_write_tmpname(path, tmp, sizeof(tmp));
    FILE *f = xis_write_open(tmp, path);
    if (!f) {
        g_free(path);
        return FALSE;
    }
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    fprintf(f, "# kiterm settings (written by the preferences dialog; hand edits are kept\n"
               "# until the next save)\n");
    fprintf(f, "font=%s\nscrollback=%d\n", c->font, c->scrollback);
    fprintf(f, "opacity=%s\n", g_ascii_dtostr(num, sizeof(num), c->opacity));
    put_color(f, "foreground", &c->fg);
    put_color(f, "background", &c->bg);
    put_color(f, "cursor_color", &c->cursor);
    for (int i = 0; i < 16; i++) {
        char key[16];
        g_snprintf(key, sizeof(key), "color%d", i);
        put_color(f, key, &c->palette[i]);
    }
    fprintf(f, "cursor_shape=%s\ncursor_blink=%d\naudible_bell=%d\nurgent_on_bell=%d\n",
            kShapes[CLAMP(c->cursor_shape, 0, 2)], c->cursor_blink, c->audible_bell, c->urgent_on_bell);
    fprintf(f, "notify_done=%d\nnotify_done_secs=%d\n", c->notify_done, c->notify_done_secs);
    fprintf(f, "scroll_on_output=%d\nscroll_on_keystroke=%d\nconfirm_close=%d\n",
            c->scroll_on_output, c->scroll_on_keystroke, c->confirm_close);
    fprintf(f, "tabs=%s\nshell=%s\nword_chars=%s\ncolumns=%d\nrows=%d\n",
            kTabsPos[CLAMP(c->tabs_pos, 0, 3)], c->shell, c->word_chars, c->cols, c->rows);
    gboolean ok = xis_write_commit(f, tmp, path, xis_check_conf_clean) == 0;
    g_free(path);
    return ok;
}
