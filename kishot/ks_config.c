/* ks_config.c - $XDG_CONFIG_HOME/kishot.conf (fallback ~/.config/kishot.conf),
 * flat key=value like the rest of KiDesktop. Unknown keys are ignored. The
 * areas last used (one for screenshots, one for recordings) are saved too,
 * so a plain `kishot` or `kishot --record` repeats them. */
#include "ks.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../shared/xis_safefile.h"

static const char *const kAreaKeys[KS_AREA_N] = { "full", "monitor", "window", "region", "output" };
static const char *const kCopyKeys[] = { "auto", "gpu", "shm" };
static const char *const kBackendKeys[] = { "auto", "plugin", "ffmpeg" };

const char *ks_area_key(KsArea a)
{
    return a < KS_AREA_N ? kAreaKeys[a] : kAreaKeys[0];
}

static void defaults(KsConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->area = KS_AREA_FULL;
    c->rec_area = KS_AREA_FULL;
    c->pointer = FALSE;
    c->auto_save = TRUE;
    c->decorations = TRUE;
    c->shot_dir = g_strdup("$PICTURES/Kishot");
    c->shot_format = g_strdup("png");
    c->jpeg_quality = 90;
    c->editor = g_strdup("");
    c->name_pattern = g_strdup("Kishot_%Y%m%d_%H%M%S");
    c->video_dir = g_strdup("$VIDEOS");
    c->container = g_strdup("mkv");
    c->vcodec = g_strdup("auto");
    c->quality = 23;
    c->fps = 30;
    c->rec_pointer = TRUE;
    c->audio_system = TRUE;
    c->audio_mic = FALSE;
    c->rec_preview = TRUE;
    c->hide_on_record = FALSE;
    c->win_w = 760;
    c->win_h = 480;
}

static char *config_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kishot.conf", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "kishot.conf", NULL);
}

static gboolean truth(const char *v)
{
    return !g_ascii_strcasecmp(v, "1") || !g_ascii_strcasecmp(v, "true") ||
           !g_ascii_strcasecmp(v, "yes") || !g_ascii_strcasecmp(v, "on");
}

static int lookup(const char *const *keys, int n, const char *v, int fallback)
{
    for (int i = 0; i < n; i++)
        if (!g_ascii_strcasecmp(keys[i], v))
            return i;
    return fallback;
}

static void set_str(char **dst, const char *v)
{
    g_free(*dst);
    *dst = g_strdup(v);
}

void ks_config_load(KsConfig *c)
{
    defaults(c);
    char *path = config_path();
    FILE *f = xis_fopen_read(path, xis_check_usable);
    g_free(path);
    if (!f)
        return;
    char line[1024];
    gboolean have_rec_area = FALSE;
    while (fgets(line, sizeof(line), f)) {
        char *s = g_strstrip(line);
        char *eq = strchr(s, '=');
        if (!*s || *s == '#' || !eq)
            continue;
        *eq = 0;
        char *k = g_strstrip(s), *v = g_strstrip(eq + 1);
        if (!strcmp(k, "area")) c->area = lookup(kAreaKeys, KS_AREA_N, v, KS_AREA_FULL);
        else if (!strcmp(k, "area_output") && *v) set_str(&c->output, v);
        else if (!strcmp(k, "record_area_output") && *v) set_str(&c->rec_output, v);
        else if (!strcmp(k, "record_area")) {
            c->rec_area = lookup(kAreaKeys, KS_AREA_N, v, KS_AREA_FULL);
            have_rec_area = TRUE;
        }
        else if (!strcmp(k, "delay")) c->delay = CLAMP(atoi(v), 0, 60);
        else if (!strcmp(k, "pointer")) c->pointer = truth(v);
        else if (!strcmp(k, "decorations")) c->decorations = truth(v);
        else if (!strcmp(k, "auto_save")) c->auto_save = truth(v);
        else if (!strcmp(k, "auto_copy")) c->auto_copy = truth(v);
        else if (!strcmp(k, "screenshot_dir") && *v) set_str(&c->shot_dir, v);
        else if (!strcmp(k, "image_format") && *v) set_str(&c->shot_format, v);
        else if (!strcmp(k, "jpeg_quality")) c->jpeg_quality = CLAMP(atoi(v), 1, 100);
        else if (!strcmp(k, "editor")) set_str(&c->editor, v);
        else if (!strcmp(k, "name_pattern") && *v) set_str(&c->name_pattern, v);
        else if (!strcmp(k, "video_dir") && *v) set_str(&c->video_dir, v);
        else if (!strcmp(k, "container") && *v) set_str(&c->container, v);
        else if (!strcmp(k, "video_codec") && *v) set_str(&c->vcodec, v);
        else if (!strcmp(k, "quality")) c->quality = CLAMP(atoi(v), 0, 51);
        else if (!strcmp(k, "fps")) c->fps = CLAMP(atoi(v), 0, 240);
        else if (!strcmp(k, "record_pointer")) c->rec_pointer = truth(v);
        else if (!strcmp(k, "audio_system")) c->audio_system = truth(v);
        else if (!strcmp(k, "audio_mic")) c->audio_mic = truth(v);
        else if (!strcmp(k, "record_preview")) c->rec_preview = truth(v);
        else if (!strcmp(k, "hide_on_record")) c->hide_on_record = truth(v);
        else if (!strcmp(k, "copy_method")) c->copy_mode = lookup(kCopyKeys, 3, v, KS_COPY_AUTO);
        else if (!strcmp(k, "backend")) c->backend = lookup(kBackendKeys, 3, v, KS_BACKEND_AUTO);
        else if (!strcmp(k, "window_width")) c->win_w = CLAMP(atoi(v), 300, 10000);
        else if (!strcmp(k, "window_height")) c->win_h = CLAMP(atoi(v), 200, 10000);
        else if (!strcmp(k, "pane_position")) c->pane_pos = CLAMP(atoi(v), 0, 10000);
    }
    fclose(f);
    /* before 0.2 one area served both */
    if (!have_rec_area) {
        c->rec_area = c->area;
        set_str(&c->rec_output, c->output);
    }
}

void ks_config_save(const KsConfig *c)
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
        return;
    }
    fprintf(f, "# kishot settings (written by kishot)\n");
    fprintf(f, "area=%s\n", ks_area_key(c->area));
    fprintf(f, "record_area=%s\n", ks_area_key(c->rec_area));
    fprintf(f, "# connector names for area=output / record_area=output\n");
    if (c->output)
        fprintf(f, "area_output=%s\n", c->output);
    if (c->rec_output)
        fprintf(f, "record_area_output=%s\n", c->rec_output);
    fprintf(f, "delay=%d\n", c->delay);
    fprintf(f, "pointer=%d\n", c->pointer);
    fprintf(f, "decorations=%d\n", c->decorations);
    fprintf(f, "auto_save=%d\n", c->auto_save);
    fprintf(f, "auto_copy=%d\n", c->auto_copy);
    fprintf(f, "# $PICTURES / $VIDEOS / ~ expand to the XDG folders\n");
    fprintf(f, "screenshot_dir=%s\n", c->shot_dir);
    fprintf(f, "image_format=%s\n", c->shot_format);
    fprintf(f, "jpeg_quality=%d\n", c->jpeg_quality);
    fprintf(f, "# strftime() pattern, extension added automatically\n");
    fprintf(f, "editor=%s\n", c->editor);
    fprintf(f, "name_pattern=%s\n", c->name_pattern);
    fprintf(f, "video_dir=%s\n", c->video_dir);
    fprintf(f, "container=%s\n", c->container);
    fprintf(f, "video_codec=%s\n", c->vcodec);
    fprintf(f, "quality=%d\n", c->quality);
    fprintf(f, "fps=%d\n", c->fps);
    fprintf(f, "record_pointer=%d\n", c->rec_pointer);
    fprintf(f, "audio_system=%d\n", c->audio_system);
    fprintf(f, "audio_mic=%d\n", c->audio_mic);
    fprintf(f, "record_preview=%d\n", c->rec_preview);
    fprintf(f, "hide_on_record=%d\n", c->hide_on_record);
    fprintf(f, "# auto: GPU copy only when the server sets the XiS root-copy bit\n");
    fprintf(f, "copy_method=%s\n", kCopyKeys[c->copy_mode]);
    fprintf(f, "# auto: the libav plugin when it loads, else the ffmpeg command\n");
    fprintf(f, "backend=%s\n", kBackendKeys[c->backend]);
    fprintf(f, "window_width=%d\n", c->win_w);
    fprintf(f, "window_height=%d\n", c->win_h);
    fprintf(f, "pane_position=%d\n", c->pane_pos);
    xis_write_commit(f, tmp, path, xis_check_conf_clean);
    g_free(path);
}

/* "$PICTURES/Kishot" -> "/home/u/Pictures/Kishot" */
char *ks_config_expand_dir(const char *dir)
{
    struct { const char *key; GUserDirectory ud; } vars[] = {
        { "$PICTURES", G_USER_DIRECTORY_PICTURES },
        { "$VIDEOS", G_USER_DIRECTORY_VIDEOS },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(vars); i++) {
        size_t n = strlen(vars[i].key);
        if (!strncmp(dir, vars[i].key, n) && (dir[n] == 0 || dir[n] == '/')) {
            const char *base = g_get_user_special_dir(vars[i].ud);
            if (!base)
                base = g_get_home_dir();
            return g_strconcat(base, dir + n, NULL);
        }
    }
    if (dir[0] == '~' && (dir[1] == 0 || dir[1] == '/'))
        return g_strconcat(g_get_home_dir(), dir + 1, NULL);
    return g_strdup(dir);
}
