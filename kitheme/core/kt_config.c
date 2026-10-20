/* libkitheme - kitheme.conf and presets.
 *
 * Format: "key = value" lines, '#' comments. A file may start from a
 * preset ("preset = oxygen"): that preset's file is read first and the
 * file's own keys override it. Presets hold the shape only (style,
 * radius, sizes); the colors live in separate schemes ("colors = claro"),
 * same format, applied after the preset. Both are looked up in
 *   $KITHEME_PRESETS / $KITHEME_COLORS (one directory; the source tree
 *                                       when testing)
 *   $XDG_DATA_HOME/kitheme/presets|colors  (~/.local/share/kitheme/...)
 *   KITHEME_DATADIR/presets|colors         (the install prefix)
 *   /usr/share/kitheme/presets|colors
 * Numbers are parsed by hand rather than with strtod(), which follows
 * LC_NUMERIC: under pt_BR "0.6" would read as 0. */
#include "kitheme.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef KITHEME_DATADIR
#define KITHEME_DATADIR "/usr/local/share/kitheme"
#endif

#define DEFAULT_PRESET "oxygen"
#define DEFAULT_COLORS "claro"

typedef enum { K_COLOR, K_DOUBLE, K_INT, K_BOOL, K_STYLE, K_FOCUS, K_PTEXT } KeyType;

typedef struct {
    const char *name;
    KeyType type;
    size_t off;
} Key;

#define F(field) offsetof(KtTheme, field)
static const Key keys[] = {
    {"style", K_STYLE, F(style)},
    {"window_bg", K_COLOR, F(window_bg)},
    {"window_fg", K_COLOR, F(window_fg)},
    {"base_bg", K_COLOR, F(base_bg)},
    {"base_fg", K_COLOR, F(base_fg)},
    {"button_bg", K_COLOR, F(button_bg)},
    {"button_fg", K_COLOR, F(button_fg)},
    {"selection_bg", K_COLOR, F(selection_bg)},
    {"selection_fg", K_COLOR, F(selection_fg)},
    {"accent", K_COLOR, F(accent)},
    {"progress", K_COLOR, F(progress)},
    {"menu_bg", K_COLOR, F(menu_bg)},
    {"menu_fg", K_COLOR, F(menu_fg)},
    {"tooltip_bg", K_COLOR, F(tooltip_bg)},
    {"tooltip_fg", K_COLOR, F(tooltip_fg)},
    {"radius", K_DOUBLE, F(radius)},
    {"relief", K_DOUBLE, F(relief)},
    {"gradient", K_DOUBLE, F(gradient)},
    {"shadow", K_DOUBLE, F(shadow)},
    {"focus", K_FOCUS, F(focus)},
    {"hover_glow", K_BOOL, F(hover_glow)},
    {"progress_thickness", K_INT, F(progress_thickness)},
    {"progress_text", K_PTEXT, F(progress_text)},
    {"progress_stripes", K_BOOL, F(progress_stripes)},
    {"progress_animation", K_BOOL, F(progress_animation)},
    {"scrollbar_width", K_INT, F(scrollbar_width)},
    {"scrollbar_arrows", K_BOOL, F(scrollbar_arrows)},
    {"check_size", K_INT, F(check_size)},
};
#undef F
#define NKEYS (sizeof(keys) / sizeof(keys[0]))

static const char *const style_names[] = {"soft", "classic", "flat", NULL};
static const char *const focus_names[] = {"glow", "dotted", "none", NULL};
static const char *const ptext_names[] = {"auto", "always", NULL};

static KtColor rgb(int r, int g, int b)
{
    KtColor c = {r / 255.0, g / 255.0, b / 255.0, 1.0};
    return c;
}

/* Shape must match presets/oxygen.conf and colors colors/claro.conf: it is
 * what a missing preset or scheme falls back to. */
void kt_theme_defaults(KtTheme *t)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->preset, sizeof(t->preset), DEFAULT_PRESET);
    snprintf(t->colors, sizeof(t->colors), DEFAULT_COLORS);
    t->style = KT_STYLE_SOFT;
    t->window_bg = rgb(0xe0, 0xdf, 0xde);
    t->window_fg = rgb(0x14, 0x13, 0x12);
    t->base_bg = rgb(0xff, 0xff, 0xff);
    t->base_fg = rgb(0x1f, 0x1c, 0x1b);
    t->button_bg = rgb(0xe8, 0xe7, 0xe6);
    t->button_fg = rgb(0x14, 0x13, 0x12);
    t->selection_bg = rgb(0x43, 0xac, 0xe8);
    t->selection_fg = rgb(0xff, 0xff, 0xff);
    t->accent = rgb(0x3a, 0xa7, 0xdd);
    t->progress = rgb(0x43, 0xac, 0xe8);
    t->menu_bg = rgb(0xe0, 0xdf, 0xde);
    t->menu_fg = rgb(0x14, 0x13, 0x12);
    t->tooltip_bg = rgb(0x18, 0x15, 0x13);
    t->tooltip_fg = rgb(0xe7, 0xfd, 0xff);
    t->radius = 3.5;
    t->relief = 0.7;
    t->gradient = 0.6;
    t->shadow = 0.6;
    t->focus = KT_FOCUS_GLOW;
    t->hover_glow = 1;
    t->progress_thickness = 20;
    t->progress_text = KT_PTEXT_ALWAYS;
    t->progress_stripes = 0;
    t->progress_animation = 0;
    t->scrollbar_width = 15;
    t->scrollbar_arrows = 1;
    t->check_size = 16;
}

/* "12", "-3", "0.6", ".25" -- '.' only, whatever the locale. */
static int parse_double(const char *s, double *out)
{
    double sign = 1, v = 0, scale = 1;
    int digits = 0, frac = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-' || *s == '+')
        sign = *s++ == '-' ? -1 : 1;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            if (frac) {
                scale /= 10;
                v += (*s - '0') * scale;
            } else {
                v = v * 10 + (*s - '0');
            }
            digits++;
        } else if (*s == '.' && !frac) {
            frac = 1;
        } else if (*s == ' ' || *s == '\t') {
            break;
        } else {
            return 0;
        }
    }
    if (!digits)
        return 0;
    *out = sign * v;
    return 1;
}

static int parse_name(const char *v, const char *const *names, int *out)
{
    for (int i = 0; names[i]; i++) {
        if (!strcmp(v, names[i])) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static void set_key(KtTheme *t, const char *key, const char *val)
{
    for (size_t i = 0; i < NKEYS; i++) {
        if (strcmp(key, keys[i].name))
            continue;
        void *p = (char *)t + keys[i].off;
        double d;
        int n;
        switch (keys[i].type) {
        case K_COLOR:
            kt_color_parse(val, (KtColor *)p);
            break;
        case K_DOUBLE:
            if (parse_double(val, &d))
                *(double *)p = d;
            break;
        case K_INT:
        case K_BOOL:
            if (parse_double(val, &d))
                *(int *)p = (int)d;
            break;
        case K_STYLE:
            if (parse_name(val, style_names, &n))
                *(KtStyle *)p = (KtStyle)n;
            break;
        case K_FOCUS:
            if (parse_name(val, focus_names, &n))
                *(KtFocus *)p = (KtFocus)n;
            break;
        case K_PTEXT:
            if (parse_name(val, ptext_names, &n))
                *(KtProgressText *)p = (KtProgressText)n;
            break;
        }
        return;
    }
}

/* Reads `path`; with `preset_only`, only copies the `preset` and `colors`
 * keys (to `preset` and `colors`, either may be NULL) without applying
 * anything. */
static int read_file(KtTheme *t, const char *path, int preset_only, char *preset, size_t psz, char *colors, size_t csz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#')
            continue;
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(l), *val = trim(eq + 1);
        if (!strcmp(key, "preset")) {
            if (preset)
                snprintf(preset, psz, "%s", val);
            continue;
        }
        if (!strcmp(key, "colors")) {
            if (colors)
                snprintf(colors, csz, "%s", val);
            continue;
        }
        if (!preset_only)
            set_key(t, key, val);
    }
    fclose(f);
    return 1;
}

/* `sub` is "presets" or "colors"; `envvar` overrides everything. */
static int data_dirs(const char *sub, const char *envvar, char dirs[4][PATH_MAX])
{
    int n = 0;
    const char *env = getenv(envvar);
    if (env && *env)
        snprintf(dirs[n++], PATH_MAX, "%s", env);
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(dirs[n++], PATH_MAX, "%s/kitheme/%s", xdg, sub);
    else if (home)
        snprintf(dirs[n++], PATH_MAX, "%s/.local/share/kitheme/%s", home, sub);
    snprintf(dirs[n++], PATH_MAX, "%s/%s", KITHEME_DATADIR, sub);
    snprintf(dirs[n++], PATH_MAX, "/usr/share/kitheme/%s", sub);
    return n;
}

static int valid_name(const char *name)
{
    return name[0] && name[0] != '.' && !strchr(name, '/');
}

/* Reads the first `NAME.conf` found in the `sub` directories onto `t`. */
static int load_named(KtTheme *t, const char *sub, const char *envvar, const char *name)
{
    if (!valid_name(name))
        return 0;
    char dirs[4][PATH_MAX];
    int n = data_dirs(sub, envvar, dirs);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX + 80];
        if (strlen(dirs[i]) + strlen(name) + 7 > sizeof(path))
            continue;
        snprintf(path, sizeof(path), "%.4000s/%.60s.conf", dirs[i], name);
        if (read_file(t, path, 0, NULL, 0, NULL, 0))
            return 1;
    }
    return 0;
}

int kt_theme_load_preset(KtTheme *t, const char *name)
{
    kt_theme_defaults(t);
    if (!load_named(t, "presets", "KITHEME_PRESETS", name))
        return 0;
    snprintf(t->preset, sizeof(t->preset), "%s", name);
    return 1;
}

int kt_theme_load_colors(KtTheme *t, const char *name)
{
    if (!load_named(t, "colors", "KITHEME_COLORS", name))
        return 0;
    snprintf(t->colors, sizeof(t->colors), "%s", name);
    return 1;
}

int kt_theme_load_file(KtTheme *t, const char *path)
{
    char preset[64] = DEFAULT_PRESET, colors[64] = DEFAULT_COLORS;
    kt_theme_defaults(t);
    if (!read_file(t, path, 1, preset, sizeof(preset), colors, sizeof(colors)))
        return 0;
    kt_theme_load_preset(t, preset);
    kt_theme_load_colors(t, colors);
    snprintf(t->preset, sizeof(t->preset), "%s", preset);
    snprintf(t->colors, sizeof(t->colors), "%s", colors);
    read_file(t, path, 0, NULL, 0, NULL, 0);
    return 1;
}

void kt_user_conf_path(char *out, size_t sz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, sz, "%s/kitheme/kitheme.conf", xdg);
    else
        snprintf(out, sz, "%s/.config/kitheme/kitheme.conf", home ? home : "/tmp");
}

void kt_theme_load_user(KtTheme *t)
{
    char path[PATH_MAX];
    kt_user_conf_path(path, sizeof(path));
    if (!kt_theme_load_file(t, path)) {
        kt_theme_load_preset(t, DEFAULT_PRESET);
        kt_theme_load_colors(t, DEFAULT_COLORS);
    }
}

/* "0.60" -- again without the locale's decimal separator. */
static void fmt_double(double d, char *out, size_t sz)
{
    long v = (long)(d * 100 + (d < 0 ? -0.5 : 0.5));
    snprintf(out, sz, "%s%ld.%02ld", v < 0 ? "-" : "", labs(v) / 100, labs(v) % 100);
}

static int mkdir_parents(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return 0;
        *p = '/';
    }
    return 1;
}

int kt_theme_save(const KtTheme *t, const char *path)
{
    if (!mkdir_parents(path))
        return 0;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return 0;
    fprintf(f, "# KiTheme -- written by kiconf, see kitheme/README.md\n");
    fprintf(f, "preset = %s\n", t->preset);
    fprintf(f, "colors = %s\n", t->colors);
    for (size_t i = 0; i < NKEYS; i++) {
        const void *p = (const char *)t + keys[i].off;
        char buf[32];
        switch (keys[i].type) {
        case K_COLOR: {
            KtColor c = *(const KtColor *)p;
            kt_color_hex(c, buf);
            if (c.a < 1.0)
                snprintf(buf + 7, sizeof(buf) - 7, "%02x", (int)(c.a * 255 + 0.5));
            break;
        }
        case K_DOUBLE:
            fmt_double(*(const double *)p, buf, sizeof(buf));
            break;
        case K_INT:
        case K_BOOL:
            snprintf(buf, sizeof(buf), "%d", *(const int *)p);
            break;
        case K_STYLE:
            snprintf(buf, sizeof(buf), "%s", style_names[*(const KtStyle *)p]);
            break;
        case K_FOCUS:
            snprintf(buf, sizeof(buf), "%s", focus_names[*(const KtFocus *)p]);
            break;
        case K_PTEXT:
            snprintf(buf, sizeof(buf), "%s", ptext_names[*(const KtProgressText *)p]);
            break;
        }
        fprintf(f, "%s = %s\n", keys[i].name, buf);
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return 0;
    }
    return rename(tmp, path) == 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int list_names(const char *sub, const char *envvar, char names[][64], int max)
{
    char dirs[4][PATH_MAX];
    int nd = data_dirs(sub, envvar, dirs), n = 0;
    for (int i = 0; i < nd; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            size_t len = strlen(e->d_name);
            if (len < 6 || len - 5 >= 64 || strcmp(e->d_name + len - 5, ".conf") || e->d_name[0] == '.')
                continue;
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(len - 5), e->d_name);
            int dup = 0;
            for (int j = 0; j < n; j++)
                dup |= !strcmp(names[j], name);
            if (!dup)
                snprintf(names[n++], 64, "%s", name);
        }
        closedir(d);
    }
    qsort(names, (size_t)n, 64, cmp_name);
    return n;
}

int kt_list_presets(char names[][64], int max)
{
    return list_names("presets", "KITHEME_PRESETS", names, max);
}

int kt_list_colorschemes(char names[][64], int max)
{
    return list_names("colors", "KITHEME_COLORS", names, max);
}

/* The user's own scheme file: ~/.local/share/kitheme/colors/NAME.conf. */
static int user_colors_path(const char *name, char *out, size_t sz)
{
    if (!valid_name(name) || strlen(name) >= 60)
        return 0;
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, sz, "%.3000s/kitheme/colors/%s.conf", xdg, name);
    else if (home)
        snprintf(out, sz, "%.3000s/.local/share/kitheme/colors/%s.conf", home, name);
    else
        return 0;
    return 1;
}

int kt_colors_is_user(const char *name)
{
    char path[PATH_MAX];
    return user_colors_path(name, path, sizeof(path)) && access(path, F_OK) == 0;
}

int kt_colors_delete(const char *name)
{
    char path[PATH_MAX];
    return user_colors_path(name, path, sizeof(path)) && remove(path) == 0;
}

int kt_theme_save_colors(const KtTheme *t, const char *name)
{
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    if (!user_colors_path(name, path, sizeof(path)) || !mkdir_parents(path))
        return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return 0;
    fprintf(f, "# KiTheme color scheme -- written by kiconf\n");
    for (size_t i = 0; i < NKEYS; i++) {
        if (keys[i].type != K_COLOR)
            continue;
        char buf[8];
        kt_color_hex(*(const KtColor *)((const char *)t + keys[i].off), buf);
        fprintf(f, "%s = %s\n", keys[i].name, buf);
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return 0;
    }
    return rename(tmp, path) == 0;
}
