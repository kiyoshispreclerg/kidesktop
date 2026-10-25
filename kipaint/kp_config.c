/* kp_config.c - ~/.config/kipaint.conf: palette, colors 1/2, tool sizes,
 * grid, layers panel, size of new images. Read once at startup, written when the palette
 * changes and on exit. */
#include "kp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Paint's classic 28 colors. */
static const char *const DEFAULT_PALETTE[KP_PALETTE_SIZE] = {
    "#000000", "#808080", "#800000", "#808000", "#008000", "#008080", "#000080",
    "#800080", "#808040", "#004040", "#0080ff", "#004080", "#8000ff", "#804000",
    "#ffffff", "#c0c0c0", "#ff0000", "#ffff00", "#00ff00", "#00ffff", "#0000ff",
    "#ff00ff", "#ffff80", "#00ff80", "#80ffff", "#8080ff", "#ff0080", "#ff8040",
};

static char *conf_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kipaint.conf", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "kipaint.conf", NULL);
}

static gboolean parse_color(const char *s, KpColor *c)
{
    unsigned int r, g, b;
    if (sscanf(s, "#%02x%02x%02x", &r, &g, &b) != 3)
        return FALSE;
    c->r = r / 255.0;
    c->g = g / 255.0;
    c->b = b / 255.0;
    c->a = 1;
    return TRUE;
}

static void format_color(char out[8], const KpColor *c)
{
    g_snprintf(out, 8, "#%02x%02x%02x", (unsigned)(c->r * 255 + 0.5), (unsigned)(c->g * 255 + 0.5),
               (unsigned)(c->b * 255 + 0.5));
}

static void set_defaults(void)
{
    for (int i = 0; i < KP_PALETTE_SIZE; i++)
        parse_color(DEFAULT_PALETTE[i], &kp.palette[i]);
    kp.color[0] = kp.palette[0];
    kp.color[1] = kp.palette[KP_PALETTE_SIZE / 2];
    kp.sizes = g_new(int, kp_n_tools);
    for (int i = 0; i < kp_n_tools; i++)
        kp.sizes[i] = kp_tools[i]->default_size;
    kp.layers_panel = TRUE;
    kp.new_w = 800;
    kp.new_h = 600;
}

void kp_config_load(void)
{
    set_defaults();
    char *path = conf_path(), *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL)) {
        g_free(path);
        return;
    }
    char **lines = g_strsplit(data, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *eq = strchr(*l, '=');
        if (!eq || **l == '#')
            continue;
        *eq = '\0';
        char *key = g_strstrip(*l), *val = g_strstrip(eq + 1);
        int n;
        if (!strcmp(key, "color1"))
            parse_color(val, &kp.color[0]);
        else if (!strcmp(key, "color2"))
            parse_color(val, &kp.color[1]);
        else if (sscanf(key, "palette%d", &n) == 1 && n >= 0 && n < KP_PALETTE_SIZE)
            parse_color(val, &kp.palette[n]);
        else if (!strcmp(key, "grid"))
            kp.grid = atoi(val) != 0;
        else if (!strcmp(key, "layers_panel"))
            kp.layers_panel = atoi(val) != 0;
        else if (!strcmp(key, "eraser_transparent"))
            kp.eraser_transparent = atoi(val) != 0;
        else if (!strcmp(key, "new_width"))
            kp.new_w = CLAMP(atoi(val), 1, 32767);
        else if (!strcmp(key, "new_height"))
            kp.new_h = CLAMP(atoi(val), 1, 32767);
        else if (g_str_has_prefix(key, "size_")) {
            for (int i = 0; i < kp_n_tools; i++)
                if (kp_tools[i]->max_size && !strcmp(key + 5, kp_tools[i]->id))
                    kp.sizes[i] = CLAMP(atoi(val), 1, kp_tools[i]->max_size);
        }
    }
    g_strfreev(lines);
    g_free(data);
    g_free(path);
}

void kp_config_save(void)
{
    GString *s = g_string_new("# kipaint settings, written by kipaint\n");
    char c[8];
    format_color(c, &kp.color[0]);
    g_string_append_printf(s, "color1 = %s\n", c);
    format_color(c, &kp.color[1]);
    g_string_append_printf(s, "color2 = %s\n", c);
    for (int i = 0; i < KP_PALETTE_SIZE; i++) {
        format_color(c, &kp.palette[i]);
        g_string_append_printf(s, "palette%d = %s\n", i, c);
    }
    for (int i = 0; i < kp_n_tools; i++)
        if (kp_tools[i]->max_size)
            g_string_append_printf(s, "size_%s = %d\n", kp_tools[i]->id, kp.sizes[i]);
    g_string_append_printf(s, "grid = %d\nlayers_panel = %d\neraser_transparent = %d\nnew_width = %d\nnew_height = %d\n",
                           kp.grid, kp.layers_panel, kp.eraser_transparent, kp.new_w, kp.new_h);
    char *path = conf_path(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, s->str, s->len, NULL);
    g_free(dir);
    g_free(path);
    g_string_free(s, TRUE);
}
