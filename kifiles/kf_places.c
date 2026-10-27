/* kf_places.c - the sidebar: places, bookmarks and devices.
 *
 * Devices come from GIO's volume monitor (GVfs/udisks when present, plain
 * /proc/mounts otherwise). Mounted ones get a usage bar under the name; their
 * context menu unmounts -- and powers the drive off when it can -- reporting
 * when it is safe to unplug. Unmounted volumes are listed too; a click mounts
 * them. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include "../shared/xis_safefile.h"
#include <string.h>
#include <glib/gstdio.h>

enum { P_ICON, P_NAME, P_KIND, P_FILE, P_USAGE, P_OBJ, P_TIP, P_N };
enum { K_HEADER, K_PLACE, K_BOOKMARK, K_MOUNT, K_VOLUME };

#define ICON_PX 22

typedef struct {
    KfApp *app;
    GtkListStore *store;
    GtkWidget *tree;
    GVolumeMonitor *vm;
    guint rebuild_id;
    guint usage_id;
    GFile *current;
    GCancellable *cancel;
} Places;

static void rebuild(Places *pl);

/* Accepts the scrolled window kf_places_new() returns or its tree view. */
static Places *get_places(GtkWidget *w)
{
    GtkWidget *tree = g_object_get_data(G_OBJECT(w), "kf-tree");
    return g_object_get_data(G_OBJECT(tree ? tree : w), "kf-places");
}

static void places_free(gpointer data)
{
    Places *pl = data;
    if (pl->rebuild_id)
        g_source_remove(pl->rebuild_id);
    if (pl->usage_id)
        g_source_remove(pl->usage_id);
    g_cancellable_cancel(pl->cancel);
    g_object_unref(pl->cancel);
    g_signal_handlers_disconnect_by_data(pl->vm, pl);
    g_object_unref(pl->vm);
    if (pl->current)
        g_object_unref(pl->current);
    g_free(pl);
}

static void add_row(Places *pl, int kind, const char *markup, GdkPixbuf *icon, GFile *file, GObject *obj,
                    const char *tip)
{
    GtkTreeIter it;
    gtk_list_store_append(pl->store, &it);
    gtk_list_store_set(pl->store, &it, P_ICON, icon, P_NAME, markup, P_KIND, kind, P_FILE, file,
                       P_USAGE, -1, P_OBJ, obj, P_TIP, tip, -1);
}

static void add_header(Places *pl, const char *title)
{
    char *esc = g_markup_escape_text(title, -1);
    char *mk = g_strdup_printf("<b><small>%s</small></b>", esc);
    add_row(pl, K_HEADER, mk, NULL, NULL, NULL, NULL);
    g_free(mk);
    g_free(esc);
}

static void add_location(Places *pl, int kind, GFile *f, const char *label_override)
{
    GFileInfo *info = g_file_query_info(f, "standard::display-name,standard::icon", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    const char *name = label_override ? label_override : (info ? g_file_info_get_display_name(info) : NULL);
    char *fallback = name ? NULL : g_file_get_basename(f);
    char *esc = g_markup_escape_text(name ? name : fallback, -1);
    GdkPixbuf *icon = kf_icon_lookup_gicon(info ? g_file_info_get_icon(info) : NULL, ICON_PX);
    char *tip = g_file_is_native(f) ? g_file_get_path(f) : g_file_get_uri(f);
    add_row(pl, kind, esc, icon, f, NULL, tip);
    g_free(tip);
    g_free(esc);
    g_free(fallback);
    if (info)
        g_object_unref(info);
}

static void add_xdg(Places *pl, GFile *home, GUserDirectory which, const char *skip_if_home_label)
{
    (void)skip_if_home_label;
    const char *path = g_get_user_special_dir(which);
    if (!path)
        return;
    GFile *f = g_file_new_for_path(path);
    if (!g_file_equal(f, home) && g_file_query_exists(f, NULL))
        add_location(pl, K_PLACE, f, NULL);
    g_object_unref(f);
}

static GList *read_bookmarks(void)
{
    GList *out = NULL;
    char *files[2] = { g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL),
                       g_build_filename(g_get_home_dir(), ".gtk-bookmarks", NULL) };
    for (int i = 0; i < 2; i++) {
        char *text = xis_read_file_or_backup(files[i], xis_check_usable, NULL);
        if (text) {
            char **lines = g_strsplit(text, "\n", -1);
            for (char **l = lines; *l; l++) {
                char *line = g_strstrip(*l);
                if (!*line)
                    continue;
                char *sp = strchr(line, ' ');
                char *uri = sp ? g_strndup(line, sp - line) : g_strdup(line);
                gboolean dup = FALSE;
                for (GList *o = out; o; o = o->next)
                    if (!strcmp(((char **)o->data)[0], uri))
                        dup = TRUE;
                if (dup) {
                    g_free(uri);
                    continue;
                }
                char **pair = g_new0(char *, 2);
                pair[0] = uri;
                pair[1] = sp ? g_strdup(g_strstrip(sp + 1)) : NULL;
                out = g_list_append(out, pair);
            }
            g_strfreev(lines);
            free(text);
        }
        g_free(files[i]);
    }
    return out;
}

static void queue_usage(Places *pl);

static void add_device_row(Places *pl, int kind, const char *name, GIcon *gicon, GFile *root, GObject *obj)
{
    char *esc = g_markup_escape_text(name, -1);
    GdkPixbuf *icon = kf_icon_lookup_gicon(gicon, ICON_PX);
    char *tip = root ? (g_file_is_native(root) ? g_file_get_path(root) : g_file_get_uri(root)) : g_strdup(_("Not mounted -- click to mount"));
    add_row(pl, kind, esc, icon, root, obj, tip);
    g_free(tip);
    g_free(esc);
}

static gboolean do_rebuild(gpointer data)
{
    Places *pl = data;
    pl->rebuild_id = 0;
    gtk_list_store_clear(pl->store);

    GFile *home = g_file_new_for_path(g_get_home_dir());
    add_header(pl, _("Places"));
    add_location(pl, K_PLACE, home, _("Home"));
    add_xdg(pl, home, G_USER_DIRECTORY_DESKTOP, NULL);
    add_xdg(pl, home, G_USER_DIRECTORY_DOCUMENTS, NULL);
    add_xdg(pl, home, G_USER_DIRECTORY_DOWNLOAD, NULL);
    add_xdg(pl, home, G_USER_DIRECTORY_MUSIC, NULL);
    add_xdg(pl, home, G_USER_DIRECTORY_PICTURES, NULL);
    add_xdg(pl, home, G_USER_DIRECTORY_VIDEOS, NULL);
    g_object_unref(home);
    GFile *trash = g_file_new_for_uri("trash:///");
    add_location(pl, K_PLACE, trash, _("Trash"));
    g_object_unref(trash);

    GList *bm = read_bookmarks();
    if (bm) {
        add_header(pl, _("Bookmarks"));
        for (GList *l = bm; l; l = l->next) {
            char **pair = l->data;
            GFile *f = g_file_new_for_uri(pair[0]);
            add_location(pl, K_BOOKMARK, f, pair[1]);
            g_object_unref(f);
            g_strfreev(pair);
        }
        g_list_free(bm);
    }

    add_header(pl, _("Devices"));
    GFile *root = g_file_new_for_path("/");
    GIcon *hd = g_themed_icon_new("drive-harddisk");
    add_device_row(pl, K_PLACE, _("File System"), hd, root, NULL);
    g_object_unref(hd);
    g_object_unref(root);

    GList *mounts = g_volume_monitor_get_mounts(pl->vm);
    for (GList *l = mounts; l; l = l->next) {
        GMount *m = l->data;
        if (g_mount_is_shadowed(m))
            continue;
        char *name = g_mount_get_name(m);
        GIcon *ic = g_mount_get_icon(m);
        GFile *r = g_mount_get_root(m);
        add_device_row(pl, K_MOUNT, name, ic, r, G_OBJECT(m));
        g_free(name);
        if (ic)
            g_object_unref(ic);
        g_object_unref(r);
    }
    g_list_free_full(mounts, g_object_unref);

    GList *vols = g_volume_monitor_get_volumes(pl->vm);
    for (GList *l = vols; l; l = l->next) {
        GVolume *v = l->data;
        GMount *m = g_volume_get_mount(v);
        if (m) {
            g_object_unref(m);
            continue;
        }
        char *name = g_volume_get_name(v);
        GIcon *ic = g_volume_get_icon(v);
        add_device_row(pl, K_VOLUME, name, ic, NULL, G_OBJECT(v));
        g_free(name);
        if (ic)
            g_object_unref(ic);
    }
    g_list_free_full(vols, g_object_unref);

    if (pl->current)
        kf_places_set_current(pl->tree, pl->current);
    queue_usage(pl);
    return FALSE;
}

static void rebuild(Places *pl)
{
    if (!pl->rebuild_id)
        pl->rebuild_id = g_timeout_add(120, do_rebuild, pl);
}

static void on_monitor_event(GVolumeMonitor *vm, gpointer obj, gpointer data)
{
    (void)vm; (void)obj;
    rebuild(data);
}

/* ---- usage bars ---------------------------------------------------------- */

typedef struct {
    Places *pl;
    GtkTreeRowReference *ref;
    GCancellable *cancel;
} UsageCtx;

static void on_usage(GObject *src, GAsyncResult *res, gpointer data)
{
    UsageCtx *c = data;
    GFileInfo *info = g_file_query_filesystem_info_finish(G_FILE(src), res, NULL);
    if (!g_cancellable_is_cancelled(c->cancel) && info) {
        GtkTreePath *path = gtk_tree_row_reference_get_path(c->ref);
        GtkTreeIter it;
        if (path && gtk_tree_model_get_iter(GTK_TREE_MODEL(c->pl->store), &it, path)) {
            guint64 total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            guint64 free_b = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            if (total > 0) {
                int pct = (int)((total - (free_b > total ? total : free_b)) * 100 / total);
                char *f = g_format_size_full(free_b, G_FORMAT_SIZE_IEC_UNITS);
                char *t = g_format_size_full(total, G_FORMAT_SIZE_IEC_UNITS);
                char *where = NULL;
                gtk_tree_model_get(GTK_TREE_MODEL(c->pl->store), &it, P_TIP, &where, -1);
                /* keep the location line, append the numbers */
                char *base = where ? g_strdup(where) : g_strdup("");
                char *nl = strchr(base, '\n');
                if (nl)
                    *nl = 0;
                char *tip = g_strdup_printf(_("%s\n%s free of %s"), base, f, t);
                gtk_list_store_set(c->pl->store, &it, P_USAGE, pct, P_TIP, tip, -1);
                g_free(tip);
                g_free(base);
                g_free(where);
                g_free(f);
                g_free(t);
            }
        }
        if (path)
            gtk_tree_path_free(path);
    }
    if (info)
        g_object_unref(info);
    gtk_tree_row_reference_free(c->ref);
    g_object_unref(c->cancel);
    g_free(c);
}

static gboolean usage_cb(gpointer data)
{
    Places *pl = data;
    GtkTreeModel *m = GTK_TREE_MODEL(pl->store);
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        int kind;
        GFile *f = NULL;
        gtk_tree_model_get(m, &it, P_KIND, &kind, P_FILE, &f, -1);
        /* Mounts and the root file system, local only: a dead network mount
         * must not stall this. */
        gboolean want = FALSE;
        if (f && g_file_is_native(f)) {
            if (kind == K_MOUNT)
                want = TRUE;
            else if (kind == K_PLACE) {
                char *p = g_file_get_path(f);
                want = p && !strcmp(p, "/");
                g_free(p);
            }
        }
        if (want) {
            UsageCtx *c = g_new0(UsageCtx, 1);
            c->pl = pl;
            GtkTreePath *path = gtk_tree_model_get_path(m, &it);
            c->ref = gtk_tree_row_reference_new(m, path);
            gtk_tree_path_free(path);
            c->cancel = g_object_ref(pl->cancel);
            g_file_query_filesystem_info_async(f, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                               G_PRIORITY_LOW, pl->cancel, on_usage, c);
        }
        if (f)
            g_object_unref(f);
        ok = gtk_tree_model_iter_next(m, &it);
    }
    return TRUE;
}

static void queue_usage(Places *pl)
{
    usage_cb(pl);
    if (!pl->usage_id)
        pl->usage_id = g_timeout_add_seconds(30, usage_cb, pl);
}

/* ---- actions --------------------------------------------------------------- */

static void mount_op_done_open(GObject *src, GAsyncResult *res, gpointer data)
{
    Places *pl = data;
    GError *err = NULL;
    if (g_volume_mount_finish(G_VOLUME(src), res, &err)) {
        GMount *m = g_volume_get_mount(G_VOLUME(src));
        if (m) {
            GFile *r = g_mount_get_root(m);
            kf_app_navigate(pl->app, r);
            g_object_unref(r);
            g_object_unref(m);
        }
    } else if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
        kf_app_message(pl->app, GTK_MESSAGE_ERROR, _("Could not mount the device"), err->message);
    }
    g_clear_error(&err);
}

static void mount_volume(Places *pl, GVolume *v)
{
    GMountOperation *mo = gtk_mount_operation_new(GTK_WINDOW(pl->app->win));
    g_volume_mount(v, G_MOUNT_MOUNT_NONE, mo, NULL, mount_op_done_open, pl);
    g_object_unref(mo);
}

typedef struct {
    Places *pl;
    char *name;
    gboolean eject;
} RemoveCtx;

static void remove_ctx_free(RemoveCtx *c)
{
    g_free(c->name);
    g_free(c);
}

static void on_unmounted(GObject *src, GAsyncResult *res, gpointer data)
{
    RemoveCtx *c = data;
    GError *err = NULL;
    gboolean ok = c->eject ? g_mount_eject_with_operation_finish(G_MOUNT(src), res, &err)
                           : g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err);
    if (ok) {
        if (c->eject)
            xis_sound_play("device-removed");
        char *msg = c->eject ? g_strdup_printf(_("\"%s\" can now be safely removed."), c->name)
                             : g_strdup_printf(_("\"%s\" was unmounted."), c->name);
        kf_app_message(c->pl->app, GTK_MESSAGE_INFO, msg, NULL);
        g_free(msg);
    } else if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
        char *msg = g_strdup_printf(_("Could not remove \"%s\""), c->name);
        kf_app_message(c->pl->app, GTK_MESSAGE_ERROR, msg, err->message);
        g_free(msg);
    }
    g_clear_error(&err);
    remove_ctx_free(c);
}

static void on_volume_ejected(GObject *src, GAsyncResult *res, gpointer data)
{
    RemoveCtx *c = data;
    GError *err = NULL;
    if (g_volume_eject_with_operation_finish(G_VOLUME(src), res, &err)) {
        xis_sound_play("device-removed");
        char *msg = g_strdup_printf(_("\"%s\" can now be safely removed."), c->name);
        kf_app_message(c->pl->app, GTK_MESSAGE_INFO, msg, NULL);
        g_free(msg);
    } else if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
        kf_app_message(c->pl->app, GTK_MESSAGE_ERROR, _("Could not remove the device"), err->message);
    }
    g_clear_error(&err);
    remove_ctx_free(c);
}

/* Unmount; for removable media also eject/power off ("safe removal"). */
static void remove_device(Places *pl, GObject *obj, gboolean prefer_eject)
{
    GMountOperation *mo = gtk_mount_operation_new(GTK_WINDOW(pl->app->win));
    RemoveCtx *c = g_new0(RemoveCtx, 1);
    c->pl = pl;
    if (G_IS_MOUNT(obj)) {
        GMount *m = G_MOUNT(obj);
        c->name = g_mount_get_name(m);
        c->eject = prefer_eject && g_mount_can_eject(m);
        if (c->eject)
            g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, mo, NULL, on_unmounted, c);
        else
            g_mount_unmount_with_operation(m, G_MOUNT_UNMOUNT_NONE, mo, NULL, on_unmounted, c);
    } else if (G_IS_VOLUME(obj)) {
        GVolume *v = G_VOLUME(obj);
        c->name = g_volume_get_name(v);
        g_volume_eject_with_operation(v, G_MOUNT_UNMOUNT_NONE, mo, NULL, on_volume_ejected, c);
    } else {
        remove_ctx_free(c);
    }
    g_object_unref(mo);
}

static gboolean path_row(Places *pl, int x, int y, GtkTreePath **path)
{
    return gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(pl->tree), x, y, path, NULL, NULL, NULL);
}

static void activate_row(Places *pl, GtkTreeIter *it)
{
    int kind;
    GFile *f = NULL;
    GObject *obj = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(pl->store), it, P_KIND, &kind, P_FILE, &f, P_OBJ, &obj, -1);
    if (kind == K_VOLUME && obj)
        mount_volume(pl, G_VOLUME(obj));
    else if (f)
        kf_app_navigate(pl->app, f);
    if (f)
        g_object_unref(f);
    if (obj)
        g_object_unref(obj);
}

static void m_open(GtkMenuItem *mi, gpointer d)
{
    (void)d;
    Places *pl = g_object_get_data(G_OBJECT(mi), "kf-pl");
    GtkTreeIter it;
    GtkTreePath *p = g_object_get_data(G_OBJECT(mi), "kf-path");
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(pl->store), &it, p))
        activate_row(pl, &it);
}

static void m_remove(GtkMenuItem *mi, gpointer d)
{
    Places *pl = g_object_get_data(G_OBJECT(mi), "kf-pl");
    GObject *obj = g_object_get_data(G_OBJECT(mi), "kf-obj");
    remove_device(pl, obj, GPOINTER_TO_INT(d));
}

static void m_unbookmark(GtkMenuItem *mi, gpointer d)
{
    (void)d;
    Places *pl = g_object_get_data(G_OBJECT(mi), "kf-pl");
    GFile *f = g_object_get_data(G_OBJECT(mi), "kf-file");
    char *uri = g_file_get_uri(f);
    char *path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    char *text = xis_read_file_or_backup(path, xis_check_usable, NULL);
    if (text) {
        GString *out = g_string_new(NULL);
        char **lines = g_strsplit(text, "\n", -1);
        for (char **l = lines; *l; l++) {
            gboolean match = g_str_has_prefix(*l, uri) && ((*l)[strlen(uri)] == 0 || (*l)[strlen(uri)] == ' ');
            if (!match && **l)
                g_string_append_printf(out, "%s\n", *l);
        }
        xis_write_file(path, out->str, out->len, xis_check_lines);
        g_string_free(out, TRUE);
        g_strfreev(lines);
        free(text);
    }
    g_free(path);
    g_free(uri);
    rebuild(pl);
}

static GtkWidget *menu_item(const char *label, GCallback cb, Places *pl, gpointer data)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    g_object_set_data(G_OBJECT(mi), "kf-pl", pl);
    g_signal_connect(mi, "activate", cb, data);
    return mi;
}

static void popup_menu(Places *pl, GtkTreePath *path, GdkEventButton *ev)
{
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(pl->store), &it, path))
        return;
    int kind;
    GFile *f = NULL;
    GObject *obj = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it, P_KIND, &kind, P_FILE, &f, P_OBJ, &obj, -1);
    if (kind == K_HEADER) {
        return;
    }
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *open = menu_item(kind == K_VOLUME ? _("_Mount") : _("_Open"), G_CALLBACK(m_open), pl, NULL);
    g_object_set_data_full(G_OBJECT(open), "kf-path", gtk_tree_path_copy(path), (GDestroyNotify)gtk_tree_path_free);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
    if (kind == K_MOUNT && obj) {
        GtkWidget *mi;
        if (g_mount_can_unmount(G_MOUNT(obj))) {
            mi = menu_item(_("_Unmount"), G_CALLBACK(m_remove), pl, GINT_TO_POINTER(0));
            g_object_set_data_full(G_OBJECT(mi), "kf-obj", g_object_ref(obj), g_object_unref);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        }
        if (g_mount_can_eject(G_MOUNT(obj))) {
            mi = menu_item(_("Safely _Remove"), G_CALLBACK(m_remove), pl, GINT_TO_POINTER(1));
            g_object_set_data_full(G_OBJECT(mi), "kf-obj", g_object_ref(obj), g_object_unref);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        }
    } else if (kind == K_VOLUME && obj && g_volume_can_eject(G_VOLUME(obj))) {
        GtkWidget *mi = menu_item(_("Safely _Remove"), G_CALLBACK(m_remove), pl, GINT_TO_POINTER(1));
        g_object_set_data_full(G_OBJECT(mi), "kf-obj", g_object_ref(obj), g_object_unref);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    } else if (kind == K_BOOKMARK && f) {
        GtkWidget *mi = menu_item(_("Remove _Bookmark"), G_CALLBACK(m_unbookmark), pl, NULL);
        g_object_set_data_full(G_OBJECT(mi), "kf-file", g_object_ref(f), g_object_unref);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    if (f)
        g_object_unref(f);
    if (obj)
        g_object_unref(obj);
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    Places *pl = data;
    GtkTreePath *path = NULL;
    if (ev->button != 1 || !path_row(pl, (gint)ev->x, (gint)ev->y, &path))
        return FALSE;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(pl->store), &it, path)) {
        int kind;
        gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it, P_KIND, &kind, -1);
        if (kind != K_HEADER)
            activate_row(pl, &it);
    }
    gtk_tree_path_free(path);
    return FALSE;
}

static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    Places *pl = data;
    if (ev->button != 3)
        return FALSE;
    GtkTreePath *path = NULL;
    if (path_row(pl, (gint)ev->x, (gint)ev->y, &path)) {
        popup_menu(pl, path, ev);
        gtk_tree_path_free(path);
        return TRUE;
    }
    return FALSE;
}

/* ---- drops onto the sidebar ---------------------------------------------------- */

/* The location a drop at (x, y) would go to: places, bookmarks and mounted
 * devices; never headers or unmounted volumes. */
static GFile *drop_row(Places *pl, gint x, gint y, GtkTreePath **path_out)
{
    GtkTreePath *path = NULL;
    gint bx, by;
    gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(pl->tree), x, y, &bx, &by);
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(pl->tree), bx, by, &path, NULL, NULL, NULL))
        return NULL;
    GtkTreeIter it;
    GFile *f = NULL;
    int kind = K_HEADER;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(pl->store), &it, path))
        gtk_tree_model_get(GTK_TREE_MODEL(pl->store), &it, P_KIND, &kind, P_FILE, &f, -1);
    if (kind == K_HEADER || kind == K_VOLUME || !f) {
        gtk_tree_path_free(path);
        if (f)
            g_object_unref(f);
        return NULL;
    }
    *path_out = path;
    return f;
}

static gboolean on_dnd_motion(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint time, gpointer data)
{
    Places *pl = data;
    GtkTreePath *path = NULL;
    GFile *f = gtk_drag_dest_find_target(w, ctx, NULL) != GDK_NONE ? drop_row(pl, x, y, &path) : NULL;
    if (!f) {
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
        gdk_drag_status(ctx, 0, time);
        return TRUE;
    }
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
    gtk_tree_path_free(path);
    /* Dropping on the trash always means "move to trash". */
    gdk_drag_status(ctx, g_file_has_uri_scheme(f, "trash") ? GDK_ACTION_MOVE : kf_dnd_default_action(pl->app, f, ctx), time);
    g_object_unref(f);
    return TRUE;
}

static void on_dnd_leave(GtkWidget *w, GdkDragContext *ctx, guint time, gpointer data)
{
    (void)ctx; (void)time; (void)data;
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
}

static gboolean on_dnd_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint time, gpointer data)
{
    Places *pl = data;
    GdkAtom target = gtk_drag_dest_find_target(w, ctx, NULL);
    GtkTreePath *path = NULL;
    GFile *f = target != GDK_NONE ? drop_row(pl, x, y, &path) : NULL;
    if (!f)
        return FALSE;
    gtk_tree_path_free(path);
    g_object_set_data_full(G_OBJECT(w), "kf-drop-dest", f, g_object_unref);
    gtk_drag_get_data(w, ctx, target, time);
    return TRUE;
}

static void on_dnd_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel,
                            guint info, guint time, gpointer data)
{
    (void)x; (void)y; (void)info;
    Places *pl = data;
    GFile *dest = g_object_get_data(G_OBJECT(w), "kf-drop-dest");
    gboolean ok = FALSE;
    if (dest && g_file_has_uri_scheme(dest, "trash")) {
        char **uris = gtk_selection_data_get_uris(sel);
        GList *files = NULL;
        for (char **u = uris; u && *u; u++)
            files = g_list_append(files, g_file_new_for_uri(*u));
        g_strfreev(uris);
        ok = files != NULL;
        kf_ops_trash(pl->app, files);
        kf_files_free(files);
    } else if (dest) {
        ok = kf_dnd_drop_uris(pl->app, sel, ctx, dest);
    }
    g_object_set_data(G_OBJECT(w), "kf-drop-dest", NULL);
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
    gtk_drag_finish(ctx, ok, FALSE, time);
}

static gboolean select_func(GtkTreeSelection *s, GtkTreeModel *m, GtkTreePath *path, gboolean cur, gpointer d)
{
    (void)s; (void)cur; (void)d;
    GtkTreeIter it;
    int kind = K_HEADER;
    if (gtk_tree_model_get_iter(m, &it, path))
        gtk_tree_model_get(m, &it, P_KIND, &kind, -1);
    return kind != K_HEADER;
}

void kf_places_set_current(GtkWidget *w, GFile *dir)
{
    Places *pl = get_places(w);
    if (pl->current != dir) {
        if (pl->current)
            g_object_unref(pl->current);
        pl->current = g_object_ref(dir);
    }
    GtkTreeModel *m = GTK_TREE_MODEL(pl->store);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(pl->tree));
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    while (ok) {
        GFile *f = NULL;
        gtk_tree_model_get(m, &it, P_FILE, &f, -1);
        if (f && g_file_equal(f, dir)) {
            gtk_tree_selection_select_iter(sel, &it);
            g_object_unref(f);
            return;
        }
        if (f)
            g_object_unref(f);
        ok = gtk_tree_model_iter_next(m, &it);
    }
    gtk_tree_selection_unselect_all(sel);
}

void kf_places_add_bookmark(GtkWidget *w, GFile *dir)
{
    Places *pl = get_places(w);
    char *path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    char *d = g_path_get_dirname(path);
    g_mkdir_with_parents(d, 0755);
    g_free(d);
    char *uri = g_file_get_uri(dir);
    char *text = xis_read_file_or_backup(path, xis_check_usable, NULL);
    GString *out = g_string_new(text ? text : "");
    if (out->len && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');
    g_string_append_printf(out, "%s\n", uri);
    xis_write_file(path, out->str, out->len, xis_check_lines);
    g_string_free(out, TRUE);
    free(text);
    g_free(uri);
    g_free(path);
    rebuild(pl);
}

GtkWidget *kf_places_new(KfApp *app)
{
    Places *pl = g_new0(Places, 1);
    pl->app = app;
    pl->cancel = g_cancellable_new();
    pl->store = gtk_list_store_new(P_N, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_INT, G_TYPE_OBJECT, G_TYPE_INT,
                                   G_TYPE_OBJECT, G_TYPE_STRING);
    pl->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(pl->store));
    g_object_unref(pl->store);
    GtkTreeView *tv = GTK_TREE_VIEW(pl->tree);
    gtk_tree_view_set_headers_visible(tv, FALSE);
    gtk_tree_view_set_tooltip_column(tv, P_TIP);
    gtk_tree_view_set_enable_search(tv, FALSE);
    g_object_set_data_full(G_OBJECT(pl->tree), "kf-places", pl, places_free);

    GtkTreeViewColumn *c1 = gtk_tree_view_column_new();
    GtkCellRenderer *ir = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(c1, ir, FALSE);
    gtk_tree_view_column_add_attribute(c1, ir, "pixbuf", P_ICON);
    GtkCellRenderer *tr = kf_usage_cell_new();        /* the name, and the usage bar under it */
    g_object_set(tr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(c1, tr, TRUE);
    gtk_tree_view_column_add_attribute(c1, tr, "markup", P_NAME);
    gtk_tree_view_column_add_attribute(c1, tr, "usage", P_USAGE);
    gtk_tree_view_column_set_expand(c1, TRUE);
    gtk_tree_view_append_column(tv, c1);

    gtk_tree_selection_set_select_function(gtk_tree_view_get_selection(tv), select_func, NULL, NULL);
    static const GtkTargetEntry dst[] = { { (gchar *)"text/uri-list", 0, 0 } };
    gtk_drag_dest_set(pl->tree, 0, dst, 1, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(pl->tree, "drag-motion", G_CALLBACK(on_dnd_motion), pl);
    g_signal_connect(pl->tree, "drag-leave", G_CALLBACK(on_dnd_leave), pl);
    g_signal_connect(pl->tree, "drag-drop", G_CALLBACK(on_dnd_drop), pl);
    g_signal_connect(pl->tree, "drag-data-received", G_CALLBACK(on_dnd_received), pl);
    g_signal_connect(tv, "button-release-event", G_CALLBACK(on_button_release), pl);
    g_signal_connect(tv, "button-press-event", G_CALLBACK(on_button_press), pl);

    pl->vm = g_volume_monitor_get();
    const char *sigs[] = { "mount-added", "mount-removed", "mount-changed", "volume-added", "volume-removed",
                           "volume-changed", "drive-connected", "drive-disconnected", "drive-changed" };
    for (guint i = 0; i < G_N_ELEMENTS(sigs); i++)
        g_signal_connect(pl->vm, sigs[i], G_CALLBACK(on_monitor_event), pl);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), pl->tree);
    g_object_set_data(G_OBJECT(sw), "kf-tree", pl->tree);
    do_rebuild(pl);
    return sw;
}
