/* kf_props.c - the Properties dialog: a General tab (text from kifiles.c) and a
 * Permissions tab that works on one or many items.
 *
 * Permissions are shown as who (owner / group / others) and what they may do
 * (read and write, read only, ...) instead of octal. With several items whose
 * values differ, a field shows "(varied)" and is left alone unless the user
 * picks something, which is then applied to every item. Changes the user may
 * not make (not the owner, or any chown) are retried as root through pkexec. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *varied_str(void) { return _("(varied)"); }

static void kf_message_parent(GtkWindow *parent, const char *msg)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

typedef struct {
    char *path;
    guint32 mode, uid, gid;
    gboolean is_dir;
    gboolean inherited;            /* found by recursion, not selected */
} PItem;

typedef struct {
    GArray *items;                 /* PItem */
    gboolean varied[3];            /* read/write differs, per class */
    gboolean x_varied[3];
    gboolean any_dir, any_file;
    gboolean owner_varied, group_varied;
    char *owner, *group;           /* shared names, or NULL when varied */
    gboolean need_root;
    GtkWidget *owner_combo, *group_combo;
    GtkWidget *access[3];
    GtkWidget *exec[3];
    GtkWidget *recursive;          /* only when a folder is selected */
} PState;

static const int rw_vals[4] = { 6, 4, 2, 0 };
static const int shifts[3] = { 6, 3, 0 };

static char *user_name(guint32 uid)
{
    struct passwd *pw = getpwuid(uid);
    return pw ? g_strdup(pw->pw_name) : g_strdup_printf("%u", uid);
}

static char *group_name(guint32 gid)
{
    struct group *gr = getgrgid(gid);
    return gr ? g_strdup(gr->gr_name) : g_strdup_printf("%u", gid);
}

static void items_free(PState *st)
{
    for (guint i = 0; i < st->items->len; i++)
        g_free(g_array_index(st->items, PItem, i).path);
    g_array_free(st->items, TRUE);
}

static void gather(PState *st, GList *files)
{
    for (GList *l = files; l; l = l->next) {
        GFileInfo *info = g_file_query_info(l->data, "standard::type,unix::mode,unix::uid,unix::gid",
                                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (!info)
            continue;
        PItem it = { 0 };
        it.path = g_file_get_path(l->data);
        it.mode = g_file_info_get_attribute_uint32(info, "unix::mode") & 07777;
        it.uid = g_file_info_get_attribute_uint32(info, "unix::uid");
        it.gid = g_file_info_get_attribute_uint32(info, "unix::gid");
        it.is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        g_object_unref(info);
        if (!it.path)
            continue;
        g_array_append_val(st->items, it);
        if (it.is_dir)
            st->any_dir = TRUE;
        else
            st->any_file = TRUE;
        if (it.uid != (guint32)geteuid() && geteuid() != 0)
            st->need_root = TRUE;
    }
    PItem *f = &g_array_index(st->items, PItem, 0);
    for (guint i = 1; i < st->items->len; i++) {
        PItem *it = &g_array_index(st->items, PItem, i);
        for (int c = 0; c < 3; c++) {
            if (((it->mode >> shifts[c]) & 6) != ((f->mode >> shifts[c]) & 6))
                st->varied[c] = TRUE;
            if (((it->mode >> shifts[c]) & 1) != ((f->mode >> shifts[c]) & 1))
                st->x_varied[c] = TRUE;
        }
        if (it->uid != f->uid)
            st->owner_varied = TRUE;
        if (it->gid != f->gid)
            st->group_varied = TRUE;
    }
    st->owner = st->owner_varied ? NULL : user_name(f->uid);
    st->group = st->group_varied ? NULL : group_name(f->gid);
}

/* ---- widgets ------------------------------------------------------------------------ */

static void on_exec_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    gtk_toggle_button_set_inconsistent(b, FALSE);
}

static GtkWidget *name_combo(gboolean groups, const char *current)
{
    GtkWidget *cb = gtk_combo_box_entry_new_text();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    if (current)
        g_ptr_array_add(names, g_strdup(current));
    if (groups) {
        setgrent();
        for (struct group *g; (g = getgrent());)
            if (g->gr_gid == 0 || (g->gr_gid >= 1000 && g->gr_gid < 65534))
                g_ptr_array_add(names, g_strdup(g->gr_name));
        endgrent();
    } else {
        setpwent();
        for (struct passwd *p; (p = getpwent());)
            if (p->pw_uid == 0 || (p->pw_uid >= 1000 && p->pw_uid < 65534))
                g_ptr_array_add(names, g_strdup(p->pw_name));
        endpwent();
    }
    for (guint i = 0; i < names->len; i++) {
        const char *n = names->pdata[i];
        if (g_hash_table_contains(seen, n))
            continue;
        g_hash_table_add(seen, (gpointer)n);
        gtk_combo_box_append_text(GTK_COMBO_BOX(cb), n);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_free(names, TRUE);
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(cb))), current ? current : varied_str());
    return cb;
}

static const char *combo_entry_text(GtkWidget *cb)
{
    return gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(cb))));
}

static GtkWidget *build_permissions(PState *st)
{
    GtkWidget *vb = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 10);

    GtkWidget *tbl = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(tbl), 6);
    gtk_table_set_col_spacings(GTK_TABLE(tbl), 10);
    st->owner_combo = name_combo(FALSE, st->owner);
    st->group_combo = name_combo(TRUE, st->group);
    GtkWidget *lo = gtk_label_new(_("Owner:")), *lg = gtk_label_new(_("Group:"));
    gtk_misc_set_alignment(GTK_MISC(lo), 0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(lg), 0, 0.5);
    gtk_table_attach(GTK_TABLE(tbl), lo, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(tbl), st->owner_combo, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(tbl), lg, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(tbl), st->group_combo, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(vb), tbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), gtk_hseparator_new(), FALSE, FALSE, 2);

    char *who[3];
    who[0] = g_strdup_printf(_("Owner (%s)"), st->owner ? st->owner : varied_str());
    who[1] = g_strdup_printf(_("Group (%s)"), st->group ? st->group : varied_str());
    who[2] = g_strdup(_("Everyone else"));
    const char *xlabel = st->any_dir && st->any_file ? _("Allow running as a program / entering the folder")
                         : st->any_dir               ? _("Allow entering the folder and listing its contents")
                                                     : _("Allow running as a program");
    const char *labels[4] = { _("Read and write"), _("Read only"), _("Write only"), _("No access") };
    PItem *f = &g_array_index(st->items, PItem, 0);

    for (int c = 0; c < 3; c++) {
        GtkWidget *fr = gtk_frame_new(who[c]);
        GtkWidget *in = gtk_vbox_new(FALSE, 4);
        gtk_container_set_border_width(GTK_CONTAINER(in), 6);
        GtkWidget *cb = gtk_combo_box_new_text();
        if (st->varied[c])
            gtk_combo_box_append_text(GTK_COMBO_BOX(cb), varied_str());
        for (int i = 0; i < 4; i++)
            gtk_combo_box_append_text(GTK_COMBO_BOX(cb), labels[i]);
        int act = 0;
        if (!st->varied[c]) {
            int cur = (f->mode >> shifts[c]) & 6;
            for (int i = 0; i < 4; i++)
                if (rw_vals[i] == cur)
                    act = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(cb), act);
        st->access[c] = cb;
        GtkWidget *ck = gtk_check_button_new_with_label(xlabel);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ck), !st->x_varied[c] && ((f->mode >> shifts[c]) & 1));
        g_signal_connect(ck, "toggled", G_CALLBACK(on_exec_toggled), NULL);
        if (st->x_varied[c])
            gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(ck), TRUE);
        st->exec[c] = ck;
        gtk_box_pack_start(GTK_BOX(in), cb, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(in), ck, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(fr), in);
        gtk_box_pack_start(GTK_BOX(vb), fr, FALSE, FALSE, 0);
        g_free(who[c]);
    }

    if (st->any_dir) {
        st->recursive = gtk_check_button_new_with_label(_("Apply to all items inside the folders"));
        gtk_box_pack_start(GTK_BOX(vb), st->recursive, FALSE, FALSE, 0);
    }
    if (st->items->len > 1) {
        GtkWidget *l = gtk_label_new(_("\"(varied)\" means the selected items differ. Leave it alone to keep each "
                                       "item's current value, or pick one to set it on all of them."));
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(vb), l, FALSE, FALSE, 0);
    }
    if (st->need_root) {
        GtkWidget *l = gtk_label_new(_("You are not the owner of some of these items. The administrator password "
                                       "will be asked to apply changes."));
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
        gtk_box_pack_start(GTK_BOX(vb), l, FALSE, FALSE, 0);
    }
    return vb;
}

/* ---- applying ----------------------------------------------------------------------- */

/* Mode a given item should get from what the dialog shows. */
static guint32 wanted_mode(PState *st, const PItem *it)
{
    guint32 m = it->mode;
    for (int c = 0; c < 3; c++) {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(st->access[c]));
        if (st->varied[c])
            idx--;                           /* 0 was "(varied)" */
        if (idx >= 0 && idx < 4)
            m = (m & ~(6u << shifts[c])) | ((guint32)rw_vals[idx] << shifts[c]);
        GtkToggleButton *ck = GTK_TOGGLE_BUTTON(st->exec[c]);
        /* The run bit chosen for a folder must not make every file inside executable. */
        if (!gtk_toggle_button_get_inconsistent(ck) && !(it->inherited && !it->is_dir))
            m = (m & ~(1u << shifts[c])) | (gtk_toggle_button_get_active(ck) ? (1u << shifts[c]) : 0);
    }
    return m;
}


static void error_box(GtkWindow *parent, const char *primary, const char *secondary)
{
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* Runs the script as root with pkexec. It goes through stdin, not argv or a
 * temp file: argv is capped (~128 KiB) and a file in the user's own space could
 * be swapped while the password prompt is open. */
static gboolean run_as_root(GtkWindow *parent, const char *script)
{
    char *argv[] = { "pkexec", "/bin/sh", "-s", NULL };
    GPid pid;
    int in_fd = -1;
    GError *err = NULL;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid,
                                  &in_fd, NULL, NULL, &err)) {
        error_box(parent, _("Could not change the permissions"), err ? err->message : NULL);
        g_clear_error(&err);
        return FALSE;
    }
    size_t len = strlen(script), off = 0;
    while (off < len) {
        ssize_t n = write(in_fd, script + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(in_fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    g_spawn_close_pid(pid);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return TRUE;
    /* pkexec: 126 = not authorized / prompt dismissed, 127 = could not run.
     * Cancelling the prompt is not worth an error box. */
    if (!(WIFEXITED(status) && (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127)))
        error_box(parent, _("Could not change the permissions"), _("The administrator command failed."));
    return FALSE;
}

/* ---- recursion ---------------------------------------------------------------------- */

/* Everything below `path`, parents before children. Symlinks are listed
 * nowhere: chmod would follow them to something outside the tree. */
static void scan_tree(GArray *out, const char *path, guint *unreadable)
{
    DIR *d = opendir(path);
    if (!d) {
        (*unreadable)++;
        return;
    }
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *p = g_build_filename(path, e->d_name, NULL);
        struct stat sb;
        if (lstat(p, &sb) != 0 || S_ISLNK(sb.st_mode)) {
            g_free(p);
            continue;
        }
        PItem it = { p, sb.st_mode & 07777, sb.st_uid, sb.st_gid, S_ISDIR(sb.st_mode), TRUE };
        g_array_append_val(out, it);
        if (it.is_dir)
            scan_tree(out, p, unreadable);
    }
    closedir(d);
}

static void all_free(GArray *all)
{
    for (guint i = 0; i < all->len; i++)
        g_free(g_array_index(all, PItem, i).path);
    g_array_free(all, TRUE);
}

/* Warns about what recursion would flatten or touch that is not the user's.
 * Cancel is the default. Returns TRUE to go on. */
static gboolean confirm_recursive(GtkWindow *parent, GArray *all, guint unreadable, gboolean *foreign_out)
{
    GHashTable *dm = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *fm = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint foreign = 0;
    for (guint i = 0; i < all->len; i++) {
        PItem *it = &g_array_index(all, PItem, i);
        g_hash_table_add(it->is_dir ? dm : fm, GUINT_TO_POINTER(it->mode + 1));
        if (it->uid != (guint32)geteuid() && geteuid() != 0)
            foreign++;
    }
    /* Folders and files are compared apart: a tree of 755 folders and 644
     * files is normal and not worth a warning. */
    gboolean differ = g_hash_table_size(dm) > 1 || g_hash_table_size(fm) > 1;
    g_hash_table_destroy(dm);
    g_hash_table_destroy(fm);
    *foreign_out = foreign > 0;
    if (!differ && !foreign && !unreadable)
        return TRUE;

    GString *m = g_string_new(NULL);
    if (differ)
        g_string_append(m, _("The items inside have different permissions from one another. Applying the same "
                             "permissions to all of them may not be a good idea.\n\n"));
    if (foreign) {
        char *t = g_strdup_printf(ngettext("%u item belongs to another user. Changing it needs administrator "
                                           "access, and could break something that depends on it.\n\n",
                                           "%u items belong to other users. Changing them needs administrator "
                                           "access, and could break things that depend on them.\n\n", foreign),
                                  foreign);
        g_string_append(m, t);
        g_free(t);
    }
    if (unreadable) {
        char *t = g_strdup_printf(ngettext("%u folder could not be read and its contents will be skipped.\n\n",
                                           "%u folders could not be read and their contents will be skipped.\n\n",
                                           unreadable), unreadable);
        g_string_append(m, t);
        g_free(t);
    }
    if (foreign)
        g_string_append(m, _("If you continue, the administrator password will be asked, and used only for the "
                             "items that are not yours."));
    else
        g_string_append(m, _("Do you want to continue?"));
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
                                          _("Apply to everything inside?"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", m->str);
    g_string_free(m, TRUE);
    gtk_dialog_add_button(GTK_DIALOG(d), _("Continue"), GTK_RESPONSE_ACCEPT);
    GtkWidget *cancel = gtk_dialog_add_button(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    gtk_widget_grab_focus(cancel);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_ACCEPT;
}

/* ---- applying ----------------------------------------------------------------------- */

/* Paths grouped for the root script, so a big tree is a few commands, not one
 * process per file. */
static void batch_add(GHashTable *h, gpointer key, const char *path)
{
    GPtrArray *a = g_hash_table_lookup(h, key);
    if (!a) {
        a = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(h, key, a);
    }
    g_ptr_array_add(a, g_shell_quote(path));
}

static void batch_emit(GString *s, const char *cmd, GPtrArray *a)
{
    for (guint i = 0; i < a->len; i += 100) {
        g_string_append_printf(s, "%s --", cmd);
        for (guint j = i; j < a->len && j < i + 100; j++)
            g_string_append_printf(s, " %s", (char *)a->pdata[j]);
        g_string_append(s, " || rc=1\n");
    }
}

/* Returns TRUE when the dialog may close. */
static gboolean apply(PState *st, GtkWindow *parent)
{
    const char *otext = combo_entry_text(st->owner_combo);
    const char *gtext = combo_entry_text(st->group_combo);
    gboolean set_owner = *otext && strcmp(otext, varied_str()) && (!st->owner || strcmp(otext, st->owner));
    gboolean set_group = *gtext && strcmp(gtext, varied_str()) && (!st->group || strcmp(gtext, st->group));
    uid_t nuid = (uid_t)-1;
    gid_t ngid = (gid_t)-1;
    if (set_owner) {
        struct passwd *pw = getpwnam(otext);
        if (!pw) {
            char *m = g_strdup_printf(_("There is no user called \"%s\"."), otext);
            kf_message_parent(parent, m);
            g_free(m);
            return FALSE;
        }
        nuid = pw->pw_uid;
    }
    if (set_group) {
        struct group *gr = getgrnam(gtext);
        if (!gr) {
            char *m = g_strdup_printf(_("There is no group called \"%s\"."), gtext);
            kf_message_parent(parent, m);
            g_free(m);
            return FALSE;
        }
        ngid = gr->gr_gid;
    }

    /* The selected items, then (when asked) everything inside the folders. */
    GArray *all = g_array_new(FALSE, TRUE, sizeof(PItem));
    for (guint i = 0; i < st->items->len; i++) {
        PItem it = g_array_index(st->items, PItem, i);
        it.path = g_strdup(it.path);
        g_array_append_val(all, it);
    }
    gboolean recursive = st->recursive && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->recursive));
    gboolean confirmed = FALSE;
    if (recursive) {
        GdkWindow *gw = gtk_widget_get_window(GTK_WIDGET(parent));
        GdkCursor *busy = gdk_cursor_new(GDK_WATCH);
        if (gw) {
            gdk_window_set_cursor(gw, busy);
            gdk_flush();
        }
        guint unreadable = 0;
        for (guint i = 0; i < st->items->len; i++)
            if (g_array_index(st->items, PItem, i).is_dir)
                scan_tree(all, g_array_index(st->items, PItem, i).path, &unreadable);
        if (gw)
            gdk_window_set_cursor(gw, NULL);
        gdk_cursor_unref(busy);
        gboolean foreign = FALSE;
        if (!confirm_recursive(parent, all, unreadable, &foreign)) {
            all_free(all);
            return FALSE;
        }
        confirmed = foreign;   /* that dialog already said the password is coming */
    }

    GHashTable *chmods = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_ptr_array_unref);
    GPtrArray *chowns = g_ptr_array_new_with_free_func(g_free);
    guint8 *rooted = g_malloc0(all->len);
    guint32 *cur = g_new(guint32, all->len);

    /* Ownership first: chown can clear setuid/setgid. */
    for (guint i = 0; i < all->len; i++) {
        PItem *it = &g_array_index(all, PItem, i);
        cur[i] = it->mode;
        if (((nuid != (uid_t)-1 && nuid != it->uid) || (ngid != (gid_t)-1 && ngid != it->gid)) &&
            chown(it->path, nuid, ngid) != 0) {
            g_ptr_array_add(chowns, g_shell_quote(it->path));
            rooted[i] = 1;
        }
    }
    /* Modes in two passes so the user never locks themselves out half-way:
     * first only add bits, parents before children, then remove bits,
     * children before parents. Items that need root are left to the script. */
    for (guint i = 0; i < all->len; i++) {
        PItem *it = &g_array_index(all, PItem, i);
        guint32 nm = wanted_mode(st, it);
        guint32 grow = it->mode | nm;
        if (grow != cur[i]) {
            if (chmod(it->path, grow) == 0)
                cur[i] = grow;
            else
                rooted[i] = 1;
        }
    }
    for (guint i = all->len; i-- > 0;) {
        PItem *it = &g_array_index(all, PItem, i);
        guint32 nm = wanted_mode(st, it);
        if (nm != cur[i] && (rooted[i] || chmod(it->path, nm) != 0)) {
            rooted[i] = 1;
            batch_add(chmods, GUINT_TO_POINTER(nm + 1), it->path);
        } else if (rooted[i] && nm != it->mode) {
            batch_add(chmods, GUINT_TO_POINTER(nm + 1), it->path);
        }
    }

    GString *script = g_string_new("rc=0\n");
    if (chowns->len) {
        char *spec = g_strdup_printf("%s%s%s", nuid != (uid_t)-1 ? otext : "", ngid != (gid_t)-1 ? ":" : "",
                                     ngid != (gid_t)-1 ? gtext : "");
        char *cmd = g_strdup_printf("chown %s", spec);
        batch_emit(script, cmd, chowns);
        g_free(cmd);
        g_free(spec);
    }
    GHashTableIter hi;
    gpointer k, v;
    g_hash_table_iter_init(&hi, chmods);
    while (g_hash_table_iter_next(&hi, &k, &v)) {
        char *cmd = g_strdup_printf("chmod %04o", GPOINTER_TO_UINT(k) - 1);
        batch_emit(script, cmd, v);
        g_free(cmd);
    }
    g_string_append(script, "exit $rc\n");

    gboolean ok = TRUE;
    if (chowns->len || g_hash_table_size(chmods)) {
        int r = GTK_RESPONSE_ACCEPT;
        if (!confirmed) {
            GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                                  _("Administrator access is needed"));
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
                                                     _("Some of these changes need permissions you do not have. "
                                                       "Apply them as administrator? You will be asked for the password."));
            gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("Apply as administrator"),
                                   GTK_RESPONSE_ACCEPT, NULL);
            r = gtk_dialog_run(GTK_DIALOG(d));
            gtk_widget_destroy(d);
        }
        ok = r == GTK_RESPONSE_ACCEPT && run_as_root(parent, script->str);
    }
    g_string_free(script, TRUE);
    g_hash_table_destroy(chmods);
    g_ptr_array_free(chowns, TRUE);
    g_free(rooted);
    g_free(cur);
    all_free(all);
    return ok;
}

/* ---- entry point -------------------------------------------------------------------- */

void kf_props_show(KfApp *app, const char *general_text, GList *files, gboolean permissions)
{
    GtkWindow *parent = kf_app_parent(app);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Properties"), parent, GTK_DIALOG_MODAL, NULL, NULL);
    PState st = { 0 };
    st.items = g_array_new(FALSE, TRUE, sizeof(PItem));
    if (permissions)
        gather(&st, files);
    permissions = permissions && st.items->len > 0;

    GtkWidget *nb = gtk_notebook_new();
    GtkWidget *gen = gtk_label_new(general_text);
    gtk_label_set_selectable(GTK_LABEL(gen), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(gen), TRUE);
    gtk_misc_set_alignment(GTK_MISC(gen), 0, 0);
    gtk_misc_set_padding(GTK_MISC(gen), 10, 10);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gen, gtk_label_new(_("General")));
    if (permissions)
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), build_permissions(&st), gtk_label_new(_("Permissions")));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), nb, TRUE, TRUE, 0);
    if (permissions)
        gtk_dialog_add_button(GTK_DIALOG(dlg), GTK_STOCK_APPLY, GTK_RESPONSE_APPLY);
    gtk_dialog_add_button(GTK_DIALOG(dlg), GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 440, -1);
    gtk_widget_show_all(dlg);

    for (;;) {
        int r = gtk_dialog_run(GTK_DIALOG(dlg));
        if (r != GTK_RESPONSE_APPLY)
            break;
        if (apply(&st, GTK_WINDOW(dlg)))
            break;
    }
    gtk_widget_destroy(dlg);
    g_free(st.owner);
    g_free(st.group);
    items_free(&st);
}
