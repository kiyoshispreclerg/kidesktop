/* kiconf - external tab plugins; see plugins.h for the file format. */
#include "plugins.h"

#include <gmodule.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "../shared/xis_i18n.h"

#ifndef DATADIR
#define DATADIR "/usr/local/share"
#endif

static void add_dir(GPtrArray *dirs, const char *base)
{
    char *d = g_build_filename(base, "kiconf", "tabs", NULL);
    for (guint i = 0; i < dirs->len; i++) {
        if (!strcmp(g_ptr_array_index(dirs, i), d)) {
            g_free(d);
            return;
        }
    }
    g_ptr_array_add(dirs, d);
}

static char *dup_str(GKeyFile *kf, const char *key, int localized)
{
    char *v = localized ? g_key_file_get_locale_string(kf, "Desktop Entry", key, NULL, NULL)
                        : g_key_file_get_string(kf, "Desktop Entry", key, NULL);
    if (v && !*v) {
        g_free(v);
        return NULL;
    }
    return v;
}

static KiconfPlugin *load_one(const char *dir, const char *file)
{
    char *path = g_build_filename(dir, file, NULL);
    GKeyFile *kf = g_key_file_new();
    KiconfPlugin *p = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
        !g_key_file_get_boolean(kf, "Desktop Entry", "Hidden", NULL)) {
        char *name = dup_str(kf, "Name", 1);
        char *module = dup_str(kf, "X-KiConf-Module", 0);
        char *exec = dup_str(kf, "Exec", 0);
        if (name && (module || exec)) {
            p = g_new0(KiconfPlugin, 1);
            p->id = g_strndup(file, strlen(file) - 8); /* ".desktop" */
            p->name = name;
            name = NULL;
            p->comment = dup_str(kf, "Comment", 1);
            p->icon = dup_str(kf, "Icon", 0);
            p->category = dup_str(kf, "X-KiConf-Category", 1);
            p->keywords = dup_str(kf, "X-KiConf-Keywords", 1);
            if (module) {
                p->module = g_path_is_absolute(module) ? g_strdup(module)
                                                       : g_build_filename(dir, module, NULL);
            } else {
                p->exec = exec;
                exec = NULL;
            }
        }
        g_free(name);
        g_free(module);
        g_free(exec);
    }
    g_key_file_free(kf);
    g_free(path);
    return p;
}

static gint by_name(gconstpointer a, gconstpointer b)
{
    const KiconfPlugin *pa = *(KiconfPlugin *const *)a, *pb = *(KiconfPlugin *const *)b;
    return g_utf8_collate(pa->name, pb->name);
}

KiconfPlugin **plugins_scan(int *count)
{
    GPtrArray *dirs = g_ptr_array_new();
    const char *over = g_getenv("KICONF_TABS_DIR"); /* testing/override */
    if (over && *over) {
        g_ptr_array_add(dirs, g_strdup(over));
    }
    add_dir(dirs, g_get_user_data_dir());
    const char *const *sys = g_get_system_data_dirs();
    for (; sys && *sys; sys++) {
        add_dir(dirs, *sys);
    }
    add_dir(dirs, DATADIR);

    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < dirs->len; i++) {
        const char *dir = g_ptr_array_index(dirs, i);
        GDir *d = g_dir_open(dir, 0, NULL);
        if (!d) {
            continue;
        }
        GPtrArray *found = g_ptr_array_new();
        const char *f;
        while ((f = g_dir_read_name(d))) {
            if (!g_str_has_suffix(f, ".desktop")) {
                continue;
            }
            KiconfPlugin *p = load_one(dir, f);
            if (!p) {
                continue;
            }
            int dup = 0; /* an earlier (higher priority) dir already has it */
            for (guint k = 0; k < out->len; k++) {
                if (!strcmp(((KiconfPlugin *)g_ptr_array_index(out, k))->id, p->id)) {
                    dup = 1;
                }
            }
            if (dup) {
                g_free(p->id);
                g_free(p->name);
                g_free(p);
            } else {
                g_ptr_array_add(found, p);
            }
        }
        g_dir_close(d);
        g_ptr_array_sort(found, by_name);
        for (guint k = 0; k < found->len; k++) {
            g_ptr_array_add(out, g_ptr_array_index(found, k));
        }
        g_ptr_array_free(found, TRUE);
    }
    g_ptr_array_free(dirs, TRUE);
    *count = (int)out->len;
    g_ptr_array_add(out, NULL);
    return (KiconfPlugin **)g_ptr_array_free(out, FALSE);
}

/* ---- Module= : dlopen'd, runs in kiconf's own process ------------------ */

typedef GtkWidget *(*PluginBuild)(void);

static GtkWidget *error_page(const char *fmt, const char *arg)
{
    char *msg = g_strdup_printf(fmt, arg);
    GtkWidget *l = gtk_label_new(msg);
    g_free(msg);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    return l;
}

static GtkWidget *build_module(KiconfPlugin *p)
{
    GModule *m = p->handle;
    if (!m) {
        m = g_module_open(p->module, G_MODULE_BIND_LAZY);
        if (!m) {
            return error_page(_("Nao foi possivel carregar o modulo: %s"), g_module_error());
        }
        g_module_make_resident(m); /* code may hold callbacks past unbuild */
        p->handle = m;
    }
    gpointer sym = NULL;
    if (!g_module_symbol(m, "kiconf_tab_build", &sym) || !sym) {
        return error_page(_("O modulo %s nao exporta kiconf_tab_build()"), p->module);
    }
    GtkWidget *w = ((PluginBuild)sym)();
    return w ? w : error_page(_("O modulo %s nao criou nenhuma pagina"), p->module);
}

/* ---- Exec= : separate process embedded through XEmbed ------------------ */

typedef struct {
    KiconfPlugin *p;
    GPid pid;
} Embed;

static void embed_realized(GtkWidget *socket, gpointer data)
{
    Embed *e = data;
    char xid[32];
    snprintf(xid, sizeof(xid), "%lu", (unsigned long)gtk_socket_get_id(GTK_SOCKET(socket)));

    char *cmd;
    if (strstr(e->p->exec, "%x")) {
        char **parts = g_strsplit(e->p->exec, "%x", -1);
        cmd = g_strjoinv(xid, parts);
        g_strfreev(parts);
    } else {
        cmd = g_strdup_printf("%s --plug=%s", e->p->exec, xid);
    }
    char **argv = NULL;
    GError *err = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &argv, &err) ||
        !g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &e->pid, &err)) {
        fprintf(stderr, "kiconf: plugin %s: %s\n", e->p->id, err ? err->message : "bad Exec");
        g_clear_error(&err);
    }
    g_strfreev(argv);
    g_free(cmd);
}

static void embed_destroyed(GtkWidget *socket, gpointer data)
{
    (void)socket;
    Embed *e = data;
    if (e->pid > 0) {
        kill(e->pid, SIGTERM);
        g_spawn_close_pid(e->pid); /* SIGCHLD reaping is left to GLib's watch */
    }
    g_free(e);
}

static gboolean embed_plug_removed(GtkSocket *socket, gpointer data)
{
    (void)socket;
    (void)data;
    return TRUE; /* child exited: keep the (now empty) socket, don't destroy */
}

static GtkWidget *build_exec(KiconfPlugin *p)
{
    Embed *e = g_new0(Embed, 1);
    e->p = p;
    GtkWidget *socket = gtk_socket_new();
    g_signal_connect(socket, "realize", G_CALLBACK(embed_realized), e);
    g_signal_connect(socket, "plug-removed", G_CALLBACK(embed_plug_removed), NULL);
    g_signal_connect(socket, "destroy", G_CALLBACK(embed_destroyed), e);
    return socket;
}

GtkWidget *plugin_build(KiconfPlugin *p)
{
    return p->module ? build_module(p) : build_exec(p);
}
