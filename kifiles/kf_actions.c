/* kf_actions.c - installable context-menu actions ("service menus").
 *
 * Any program can add entries to kifiles' right-click menu by installing a
 * .desktop file in the KDE service-menu format (KDE's own service menus work
 * unchanged). Nothing is loaded into kifiles: the files are declarative and
 * an action is an ordinary command, so a broken one cannot take the file
 * manager down. The format and the directories are in SERVICEMENUS.md.
 *
 * The directories are rescanned at every right-click instead of cached: a
 * handful of tiny files, no resident memory, and an action that was just
 * installed or edited shows up at once.
 */
#include "kf.h"
#include <gio/gdesktopappinfo.h>
#include "../shared/xis_i18n.h"

#include <stdlib.h>
#include <string.h>

#define GROUP "Desktop Entry"

typedef struct {
    char *id;               /* "<file>:<token>", what ki-fileactions.conf disables */
    char *name;
    char *icon;
    char *exec;
} KfAct;

typedef struct {
    char *file_id;          /* basename, the order and the override key */
    char *submenu;          /* X-KDE-Submenu, or NULL */
    gboolean toplevel;      /* X-KDE-Priority=TopLevel */
    char **mimes;
    char **protocols;       /* NULL: "file" only */
    int min_urls, max_urls; /* 0: no limit */
    int *req_urls;          /* X-KDE-RequiredNumberOfUrls, n_req entries */
    int n_req;
    char *try_exec;
    GPtrArray *acts;        /* KfAct* */
} KfService;

static void act_free(gpointer p)
{
    KfAct *a = p;
    g_free(a->id);
    g_free(a->name);
    g_free(a->icon);
    g_free(a->exec);
    g_free(a);
}

static void service_free(gpointer p)
{
    KfService *s = p;
    g_free(s->file_id);
    g_free(s->submenu);
    g_strfreev(s->mimes);
    g_strfreev(s->protocols);
    g_free(s->req_urls);
    g_free(s->try_exec);
    if (s->acts)
        g_ptr_array_free(s->acts, TRUE);
    g_free(s);
}

/* ---- the directories and the disabled list ----------------------------------- */

/* User's first, so a file there overrides a system one of the same name. */
static GPtrArray *service_dirs(void)
{
    static const char *const kSub[] = { "kifiles/actions", "kio/servicemenus", "kservices5/ServiceMenus" };
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < G_N_ELEMENTS(kSub); i++)
        g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), kSub[i], NULL));
    for (const char *const *d = g_get_system_data_dirs(); *d; d++)
        for (guint i = 0; i < G_N_ELEMENTS(kSub); i++)
            g_ptr_array_add(dirs, g_build_filename(*d, kSub[i], NULL));
    return dirs;
}

/* `disabled=<id>` lines of $XDG_CONFIG_HOME/ki-fileactions.conf (kiconf's
 * "Acoes de arquivos" tab writes it; kifiles only reads it). A bare file name
 * (no ':') disables every action of that file. */
static GHashTable *load_disabled(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *path = g_build_filename(g_get_user_config_dir(), "ki-fileactions.conf", NULL);
    char *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            char *line = g_strstrip(*l);
            if (g_str_has_prefix(line, "disabled=") && line[9])
                g_hash_table_add(set, g_strdup(line + 9));
        }
        g_strfreev(lines);
        g_free(text);
    }
    g_free(path);
    return set;
}

/* ---- parsing -------------------------------------------------------------------- */

static char **list_or_null(GKeyFile *kf, const char *key)
{
    char **v = g_key_file_get_string_list(kf, GROUP, key, NULL, NULL);
    if (v && !v[0]) {
        g_strfreev(v);
        v = NULL;
    }
    return v;
}

static KfAct *read_action(GKeyFile *kf, const char *file_id, const char *group, const char *token)
{
    char *exec = g_key_file_get_string(kf, group, "Exec", NULL);
    char *name = g_key_file_get_locale_string(kf, group, "Name", NULL, NULL);
    if (!exec || !*exec || !name || !*name) {
        g_free(exec);
        g_free(name);
        return NULL;
    }
    KfAct *a = g_new0(KfAct, 1);
    a->id = g_strdup_printf("%s:%s", file_id, token);
    a->name = name;
    a->exec = exec;
    a->icon = g_key_file_get_string(kf, group, "Icon", NULL);
    return a;
}

static KfService *parse_service(const char *path, const char *file_id)
{
    GKeyFile *kf = g_key_file_new();
    KfService *s = NULL;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) || !g_key_file_has_group(kf, GROUP))
        goto done;
    if (g_key_file_get_boolean(kf, GROUP, "Hidden", NULL))
        goto done;
    char **mimes = list_or_null(kf, "MimeType");
    if (!mimes)
        goto done;

    s = g_new0(KfService, 1);
    s->file_id = g_strdup(file_id);
    s->mimes = mimes;
    s->protocols = list_or_null(kf, "X-KDE-Protocols");
    s->try_exec = g_key_file_get_string(kf, GROUP, "TryExec", NULL);
    s->min_urls = g_key_file_get_integer(kf, GROUP, "X-KDE-MinNumberOfUrls", NULL);
    s->max_urls = g_key_file_get_integer(kf, GROUP, "X-KDE-MaxNumberOfUrls", NULL);
    char **req = list_or_null(kf, "X-KDE-RequiredNumberOfUrls");
    if (req) {
        s->n_req = g_strv_length(req);
        s->req_urls = g_new(int, s->n_req);
        for (int i = 0; i < s->n_req; i++)
            s->req_urls[i] = atoi(req[i]);
        g_strfreev(req);
    }
    char *sub = g_key_file_get_locale_string(kf, GROUP, "X-KDE-Submenu", NULL, NULL);
    if (sub && *sub)
        s->submenu = sub;
    else
        g_free(sub);
    char *prio = g_key_file_get_string(kf, GROUP, "X-KDE-Priority", NULL);
    s->toplevel = prio && !g_ascii_strcasecmp(prio, "TopLevel");
    g_free(prio);

    s->acts = g_ptr_array_new_with_free_func(act_free);
    char **tokens = list_or_null(kf, "Actions");
    if (tokens) {
        for (char **t = tokens; *t; t++) {
            char *grp = g_strdup_printf("Desktop Action %s", *t);
            KfAct *a = read_action(kf, file_id, grp, *t);
            g_free(grp);
            if (a)
                g_ptr_array_add(s->acts, a);
        }
        g_strfreev(tokens);
    } else {
        /* KDE 4 style: the entry itself is the one action */
        KfAct *a = read_action(kf, file_id, GROUP, "-");
        if (a)
            g_ptr_array_add(s->acts, a);
    }
    if (!s->acts->len) {
        service_free(s);
        s = NULL;
    }
done:
    g_key_file_free(kf);
    return s;
}

static gint cmp_service(gconstpointer a, gconstpointer b)
{
    const KfService *x = *(KfService *const *)a, *y = *(KfService *const *)b;
    return g_strcmp0(x->file_id, y->file_id);
}

static GPtrArray *load_services(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(service_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *dirs = service_dirs();
    for (guint i = 0; i < dirs->len; i++) {
        GDir *d = g_dir_open(g_ptr_array_index(dirs, i), 0, NULL);
        if (!d)
            continue;
        const char *name;
        while ((name = g_dir_read_name(d))) {
            if (!g_str_has_suffix(name, ".desktop") || g_hash_table_contains(seen, name))
                continue;
            g_hash_table_add(seen, g_strdup(name));      /* even if Hidden: that masks the later ones */
            char *path = g_build_filename(g_ptr_array_index(dirs, i), name, NULL);
            KfService *s = parse_service(path, name);
            g_free(path);
            if (s)
                g_ptr_array_add(out, s);
        }
        g_dir_close(d);
    }
    g_ptr_array_free(dirs, TRUE);
    g_hash_table_destroy(seen);
    g_ptr_array_sort(out, cmp_service);
    return out;
}

/* ---- matching ------------------------------------------------------------------- */

typedef struct {
    gboolean is_dir;
    char *ctype;
    char *scheme;
} Item;

static gboolean mime_match(const char *pat, const Item *it)
{
    if (!strcmp(pat, "all/all"))
        return TRUE;
    if (!strcmp(pat, "all/allfiles"))
        return !it->is_dir;
    const char *ct = it->is_dir ? "inode/directory" : it->ctype;
    if (!ct)
        return FALSE;
    size_t n = strlen(pat);
    if (n > 2 && pat[n - 2] == '/' && pat[n - 1] == '*' && !strncmp(ct, pat, n - 1))
        return TRUE;
    return g_content_type_is_a(ct, pat);
}

static gboolean service_applies(const KfService *s, const Item *items, int n)
{
    if (s->min_urls && n < s->min_urls)
        return FALSE;
    if (s->max_urls && n > s->max_urls)
        return FALSE;
    if (s->n_req) {
        gboolean ok = FALSE;
        for (int i = 0; i < s->n_req; i++)
            ok |= s->req_urls[i] == n;
        if (!ok)
            return FALSE;
    }
    if (s->try_exec && *s->try_exec) {
        char *p = g_path_is_absolute(s->try_exec) ? g_strdup(s->try_exec) : g_find_program_in_path(s->try_exec);
        gboolean ok = p && g_file_test(p, G_FILE_TEST_IS_EXECUTABLE);
        g_free(p);
        if (!ok)
            return FALSE;
    }
    for (int i = 0; i < n; i++) {
        gboolean proto = FALSE;
        if (!s->protocols) {
            proto = !g_strcmp0(items[i].scheme, "file");
        } else {
            for (char **p = s->protocols; *p; p++)
                proto |= !strcmp(*p, "*") || !g_strcmp0(*p, items[i].scheme);
        }
        if (!proto)
            return FALSE;
        gboolean m = FALSE;
        for (char **p = s->mimes; *p && !m; p++)
            m = mime_match(*p, &items[i]);
        if (!m)
            return FALSE;
    }
    return TRUE;
}

/* ---- running -------------------------------------------------------------------- */

typedef struct {
    KfApp *app;
    char *name;
    char *exec;
    char *dir;              /* working directory (the folder shown), or NULL */
    GList *files;           /* GFile*, the selection when the menu opened */
} Run;

static void run_free(gpointer p)
{
    Run *r = p;
    g_free(r->name);
    g_free(r->exec);
    g_free(r->dir);
    g_list_free_full(r->files, g_object_unref);
    g_free(r);
}

static void on_activate(GtkMenuItem *mi, gpointer unused)
{
    (void)unused;
    Run *r = g_object_get_data(G_OBJECT(mi), "kf-run");
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, GROUP, "Type", "Application");
    g_key_file_set_string(kf, GROUP, "Name", r->name);
    g_key_file_set_string(kf, GROUP, "Exec", r->exec);
    if (r->dir)
        g_key_file_set_string(kf, GROUP, "Path", r->dir);
    GDesktopAppInfo *ai = g_desktop_app_info_new_from_keyfile(kf);
    g_key_file_free(kf);
    GError *err = NULL;
    if (!ai) {
        kf_app_message(r->app, GTK_MESSAGE_ERROR, _("Could not run the action"), r->name);
        return;
    }
    GdkAppLaunchContext *ctx = gdk_app_launch_context_new();
    gdk_app_launch_context_set_screen(ctx, gtk_widget_get_screen(r->app->win));
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    if (!g_app_info_launch(G_APP_INFO(ai), r->files, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        kf_app_message(r->app, GTK_MESSAGE_ERROR, _("Could not run the action"), err ? err->message : r->name);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
    g_object_unref(ai);
}

/* ---- the menu --------------------------------------------------------------------- */

static GtkWidget *make_item(const char *label, const char *icon)
{
    GtkWidget *mi = gtk_image_menu_item_new_with_label(label);
    if (icon && *icon) {
        GIcon *gi = g_icon_new_for_string(icon, NULL);
        if (gi) {
            gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi), gtk_image_new_from_gicon(gi, GTK_ICON_SIZE_MENU));
            g_object_unref(gi);
        }
    }
    return mi;
}

/* The submenu called `name` inside `parent` (a GtkMenu) -- made once per
 * parent, so the actions of several files that name it share it. */
static GtkWidget *submenu_in(GHashTable *made, GtkWidget *parent, const char *name, const char *icon)
{
    char *key = g_strdup_printf("%p/%s", (void *)parent, name);
    GtkWidget *sub = g_hash_table_lookup(made, key);
    if (sub) {
        g_free(key);
        return sub;
    }
    GtkWidget *top = make_item(name, icon);
    sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(parent), top);
    g_hash_table_insert(made, key, sub);
    return sub;
}

int kf_actions_add_to_menu(KfApp *app, KfPane *p, GtkWidget *menu, GList *sel)
{
    int n = (int)g_list_length(sel);
    if (n < 1)
        return 0;
    Item *items = g_new0(Item, n);
    int i = 0;
    for (GList *l = sel; l; l = l->next, i++) {
        kf_pane_file_info(p, l->data, &items[i].is_dir, &items[i].ctype);
        items[i].scheme = g_file_get_uri_scheme(l->data);
    }

    GPtrArray *services = load_services();
    GHashTable *disabled = load_disabled();
    GHashTable *made = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *dir = p->dir && g_file_is_native(p->dir) ? g_file_get_path(p->dir) : NULL;

    /* Built into a scratch menu so the separator goes in only if there is
     * something to show; "Actions" holds what isn't asked to be top level. */
    GtkWidget *top = gtk_menu_new();
    GtkWidget *actions_menu = gtk_menu_new();
    int count = 0;
    for (guint si = 0; si < services->len; si++) {
        KfService *s = g_ptr_array_index(services, si);
        if (g_hash_table_contains(disabled, s->file_id) || !service_applies(s, items, n))
            continue;
        for (guint ai = 0; ai < s->acts->len; ai++) {
            KfAct *a = g_ptr_array_index(s->acts, ai);
            if (g_hash_table_contains(disabled, a->id))
                continue;
            GtkWidget *parent = s->toplevel ? top : actions_menu;
            if (s->submenu)
                parent = submenu_in(made, parent, s->submenu, NULL);
            GtkWidget *mi = make_item(a->name, a->icon);
            Run *r = g_new0(Run, 1);
            r->app = app;
            r->name = g_strdup(a->name);
            r->exec = g_strdup(a->exec);
            r->dir = g_strdup(dir);
            for (GList *l = sel; l; l = l->next)
                r->files = g_list_append(r->files, g_object_ref(l->data));
            g_object_set_data_full(G_OBJECT(mi), "kf-run", r, run_free);
            g_signal_connect(mi, "activate", G_CALLBACK(on_activate), NULL);
            gtk_menu_shell_append(GTK_MENU_SHELL(parent), mi);
            count++;
        }
    }
    if (count) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        GList *kids = gtk_container_get_children(GTK_CONTAINER(top));
        for (GList *l = kids; l; l = l->next) {
            g_object_ref(l->data);
            gtk_container_remove(GTK_CONTAINER(top), l->data);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), l->data);
            g_object_unref(l->data);
        }
        g_list_free(kids);
        GList *rest = gtk_container_get_children(GTK_CONTAINER(actions_menu));
        if (rest) {
            GtkWidget *ai = gtk_menu_item_new_with_label(_("Actions"));
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(ai), actions_menu);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), ai);
            actions_menu = NULL;
        }
        g_list_free(rest);
    }
    if (actions_menu)
        gtk_widget_destroy(actions_menu);
    gtk_widget_destroy(top);

    for (i = 0; i < n; i++) {
        g_free(items[i].ctype);
        g_free(items[i].scheme);
    }
    g_free(items);
    g_free(dir);
    g_hash_table_destroy(made);
    g_hash_table_destroy(disabled);
    g_ptr_array_free(services, TRUE);
    return count;
}
