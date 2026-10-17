/* kiconf - xispanel widget modules; see widgetmods.h for the file format. */
#include "widgetmods.h"

#include <stdlib.h>
#include <string.h>

#ifndef XISPANEL_WIDGETDIR
#define XISPANEL_WIDGETDIR "/usr/local/lib/xispanel/widgets"
#endif

static int type_name_ok(const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || n > 31) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '-' || t[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

static int load_field(GKeyFile *kf, const char *group, KiconfWidgetModField *f)
{
    f->key = g_strdup(group + strlen("Field "));
    while (*f->key == ' ') {
        memmove(f->key, f->key + 1, strlen(f->key));
    }
    if (!*f->key) {
        g_free(f->key);
        return 0;
    }
    char *type = g_key_file_get_string(kf, group, "Type", NULL);
    f->type = KWM_STRING;
    if (type) {
        if (!strcmp(type, "int")) {
            f->type = KWM_INT;
        } else if (!strcmp(type, "bool")) {
            f->type = KWM_BOOL;
        } else if (!strcmp(type, "enum")) {
            f->type = KWM_ENUM;
        }
        g_free(type);
    }
    f->label = g_key_file_get_locale_string(kf, group, "Name", NULL, NULL);
    if (!f->label || !*f->label) {
        g_free(f->label);
        f->label = g_strdup(f->key);
    }
    f->def = g_key_file_get_string(kf, group, "Default", NULL);
    if (!f->def) {
        f->def = g_strdup(f->type == KWM_BOOL ? "no" : f->type == KWM_INT ? "0" : "");
    }
    f->min = g_key_file_get_double(kf, group, "Min", NULL);
    f->max = g_key_file_get_double(kf, group, "Max", NULL);
    if (f->type == KWM_INT && f->max <= f->min) {
        f->max = f->min + 1000000;
    }
    if (f->type == KWM_ENUM) {
        f->opts = g_key_file_get_string_list(kf, group, "Options", NULL, NULL);
        if (!f->opts || !f->opts[0]) {
            f->type = KWM_STRING; /* an enum with no choices is just text */
        }
    }
    return 1;
}

static KiconfWidgetMod *load_one(const char *dir, const char *file)
{
    char *type = g_strndup(file, strlen(file) - strlen(".widget"));
    char *so = g_strconcat(dir, "/", type, ".so", NULL);
    char *path = g_build_filename(dir, file, NULL);
    GKeyFile *kf = g_key_file_new();
    KiconfWidgetMod *m = NULL;

    if (type_name_ok(type) && g_file_test(so, G_FILE_TEST_EXISTS) &&
        g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        m = g_new0(KiconfWidgetMod, 1);
        m->type = type;
        type = NULL;
        m->embeddable = g_key_file_get_boolean(kf, "Widget", "Embeddable", NULL);
        gsize n_groups;
        char **groups = g_key_file_get_groups(kf, &n_groups);
        m->fields = g_new0(KiconfWidgetModField, KICONF_WIDGETMOD_MAX_FIELDS);
        for (gsize i = 0; i < n_groups && m->n_fields < KICONF_WIDGETMOD_MAX_FIELDS; i++) {
            if (g_str_has_prefix(groups[i], "Field ") && load_field(kf, groups[i], &m->fields[m->n_fields])) {
                m->n_fields++;
            }
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    g_free(type);
    g_free(so);
    g_free(path);
    return m;
}

KiconfWidgetMod **widgetmods_scan(const char *const *builtin, int *count)
{
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    const char *env = g_getenv("XISPANEL_WIDGET_PATH");
    if (env && *env) {
        char **parts = g_strsplit(env, ":", -1);
        for (char **d = parts; *d; d++) {
            if (**d) {
                g_ptr_array_add(dirs, g_strdup(*d));
            }
        }
        g_strfreev(parts);
    }
    g_ptr_array_add(dirs, g_build_filename(g_get_home_dir(), ".local", "lib", "xispanel", "widgets", NULL));
    g_ptr_array_add(dirs, g_strdup(XISPANEL_WIDGETDIR));

    GPtrArray *mods = g_ptr_array_new();
    for (guint d = 0; d < dirs->len; d++) {
        const char *dir = g_ptr_array_index(dirs, d);
        GDir *gd = g_dir_open(dir, 0, NULL);
        if (!gd) {
            continue;
        }
        const char *name;
        while ((name = g_dir_read_name(gd))) {
            if (!g_str_has_suffix(name, ".widget")) {
                continue;
            }
            KiconfWidgetMod *m = load_one(dir, name);
            int dup = !m;
            for (int i = 0; !dup && builtin && builtin[i]; i++) {
                dup = !strcmp(builtin[i], m->type);
            }
            for (guint i = 0; !dup && i < mods->len; i++) {
                dup = !strcmp(((KiconfWidgetMod *)g_ptr_array_index(mods, i))->type, m->type);
            }
            if (dup) {
                continue; /* not freed: a handful of bytes, once per run */
            }
            g_ptr_array_add(mods, m);
        }
        g_dir_close(gd);
    }
    g_ptr_array_free(dirs, TRUE);
    if (count) {
        *count = (int)mods->len;
    }
    g_ptr_array_add(mods, NULL);
    return (KiconfWidgetMod **)g_ptr_array_free(mods, FALSE);
}
