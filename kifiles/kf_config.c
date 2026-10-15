/* kf_config.c - $XDG_CONFIG_HOME/kifiles.conf (fallback ~/.config/kifiles.conf),
 * flat key=value like the rest of KiDesktop. Unknown keys are ignored. */
#include "kf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void kf_config_defaults(KfConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->view = KF_VIEW_ICONS;
    c->zoom = 64;
    c->sort = KF_SORT_NAME;
    c->folders_first = TRUE;
    c->thumbnails = TRUE;
    c->show_places = TRUE;
    c->hover = TRUE;
    c->win_w = 1000;
    c->win_h = 640;
    c->places_w = 210;
}

static char *config_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kifiles.conf", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "kifiles.conf", NULL);
}

static const char *const kSortNames[] = { "name", "size", "modified", "type", "extension" };

static gboolean truth(const char *v)
{
    return !g_ascii_strcasecmp(v, "1") || !g_ascii_strcasecmp(v, "true") ||
           !g_ascii_strcasecmp(v, "yes") || !g_ascii_strcasecmp(v, "on");
}

void kf_config_load(KfConfig *c)
{
    kf_config_defaults(c);
    char *path = config_path();
    FILE *f = fopen(path, "r");
    g_free(path);
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = g_strstrip(line);
        char *eq = strchr(s, '=');
        if (!*s || *s == '#' || !eq)
            continue;
        *eq = 0;
        char *k = g_strstrip(s), *v = g_strstrip(eq + 1);
        if (!strcmp(k, "view")) c->view = !g_ascii_strcasecmp(v, "list") ? KF_VIEW_LIST : KF_VIEW_ICONS;
        else if (!strcmp(k, "zoom")) c->zoom = CLAMP(atoi(v), KF_ZOOM_MIN, KF_ZOOM_MAX);
        else if (!strcmp(k, "sort")) {
            for (int i = 0; i < KF_SORT_N; i++)
                if (!g_ascii_strcasecmp(kSortNames[i], v))
                    c->sort = i;
        }
        else if (!strcmp(k, "sort_desc")) c->sort_desc = truth(v);
        else if (!strcmp(k, "folders_first")) c->folders_first = truth(v);
        else if (!strcmp(k, "show_hidden")) c->show_hidden = truth(v);
        else if (!strcmp(k, "thumbnails")) c->thumbnails = truth(v);
        else if (!strcmp(k, "split")) c->split = truth(v);
        else if (!strcmp(k, "show_places")) c->show_places = truth(v);
        else if (!strcmp(k, "hover_highlight")) c->hover = truth(v);
        else if (!strcmp(k, "window_width")) c->win_w = CLAMP(atoi(v), 300, 10000);
        else if (!strcmp(k, "window_height")) c->win_h = CLAMP(atoi(v), 200, 10000);
        else if (!strcmp(k, "places_width")) c->places_w = CLAMP(atoi(v), 80, 600);
    }
    fclose(f);
}

void kf_config_save(const KfConfig *c)
{
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    FILE *f = fopen(path, "w");
    g_free(path);
    if (!f)
        return;
    fprintf(f, "# kifiles settings (written on exit)\n");
    fprintf(f, "view=%s\nzoom=%d\nsort=%s\nsort_desc=%d\nfolders_first=%d\nshow_hidden=%d\n",
            c->view == KF_VIEW_LIST ? "list" : "icons", c->zoom, kSortNames[CLAMP(c->sort, 0, KF_SORT_N - 1)],
            c->sort_desc, c->folders_first, c->show_hidden);
    fprintf(f, "hover_highlight=%d\n", c->hover);
    fprintf(f, "thumbnails=%d\nsplit=%d\nshow_places=%d\nwindow_width=%d\nwindow_height=%d\nplaces_width=%d\n",
            c->thumbnails, c->split, c->show_places, c->win_w, c->win_h, c->places_w);
    fclose(f);
}
