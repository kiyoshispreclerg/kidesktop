/*
 * storage.c - the --storage page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section): removable block devices (USB sticks, SD cards,
 * external drives), mount/unmount/eject. Same "shell out to the
 * system's own CLI" pattern network.c (nmcli) and pages/power.c
 * (upower/brightnessctl) already use -- `lsblk -P` for listing (stable
 * key="value" pairs, one per line, no udev/udisks2 client library
 * needed) and `udisksctl` for the actual mount/unmount/power-off, which
 * goes through udisks2's polkit rules so no setuid helper of our own is
 * needed either.
 */
#include "../xisserve.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_POLL_MS 3000

typedef struct {
    char name[64];       /* e.g. "sdb1" */
    char pkname[64];      /* parent disk, e.g. "sdb"; "" if this row IS a disk */
    char type[16];        /* "disk" or "part" */
    char size[32];
    char fstype[32];
    char label[128];
    char mountpoint[PATH_MAX];
} StorageRow;

static GtkWidget *g_root;
static GtkWidget *g_box;
static guint g_poll_id;

/* ---- lsblk -P parsing ------------------------------------------------------ */

/* One `KEY="value"` pair starting at or after *pp, value unescaped
 * (lsblk backslash-escapes '"' and '\\' inside -P output). Advances *pp
 * past it. Returns FALSE once the line is exhausted. */
static gboolean next_kv(const char **pp, char *key, size_t keysz, char *val, size_t valsz)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    if (!*p) { *pp = p; return FALSE; }
    size_t ko = 0;
    while (*p && *p != '=' && ko + 1 < keysz) key[ko++] = *p++;
    key[ko] = 0;
    if (*p != '=' || p[1] != '"') { *pp = p; return FALSE; }
    p += 2;
    size_t vo = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        if (vo + 1 < valsz) val[vo++] = *p;
        p++;
    }
    val[vo] = 0;
    if (*p == '"') p++;
    *pp = p;
    return TRUE;
}

static void apply_kv(StorageRow *r, const char *key, const char *val)
{
    if (strcmp(key, "NAME") == 0) snprintf(r->name, sizeof(r->name), "%s", val);
    else if (strcmp(key, "PKNAME") == 0) snprintf(r->pkname, sizeof(r->pkname), "%s", val);
    else if (strcmp(key, "TYPE") == 0) snprintf(r->type, sizeof(r->type), "%s", val);
    else if (strcmp(key, "SIZE") == 0) snprintf(r->size, sizeof(r->size), "%s", val);
    else if (strcmp(key, "FSTYPE") == 0) snprintf(r->fstype, sizeof(r->fstype), "%s", val);
    else if (strcmp(key, "LABEL") == 0) snprintf(r->label, sizeof(r->label), "%s", val);
    else if (strcmp(key, "MOUNTPOINT") == 0) snprintf(r->mountpoint, sizeof(r->mountpoint), "%s", val);
}

/* Removable + hotplug block devices, partitions before their own disk
 * row is skipped (a bare unpartitioned removable disk with its own
 * filesystem -- FSTYPE set directly on the "disk" row -- is kept; a
 * partitioned disk's own "disk" row, which has no FSTYPE, is dropped in
 * favor of listing its "part" rows individually). */
static GPtrArray *fetch_storage(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    FILE *f = popen("LC_ALL=C lsblk -P -o NAME,PKNAME,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINT,RM,HOTPLUG 2>/dev/null", "r");
    if (!f) return rows;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        StorageRow r;
        memset(&r, 0, sizeof(r));
        char removable[8] = "0", hotplug[8] = "0";
        const char *p = line;
        char key[32], val[PATH_MAX];
        while (next_kv(&p, key, sizeof(key), val, sizeof(val))) {
            if (strcmp(key, "RM") == 0) snprintf(removable, sizeof(removable), "%s", val);
            else if (strcmp(key, "HOTPLUG") == 0) snprintf(hotplug, sizeof(hotplug), "%s", val);
            else apply_kv(&r, key, val);
        }
        gboolean is_removable = strcmp(removable, "1") == 0 || strcmp(hotplug, "1") == 0;
        if (!is_removable) continue;
        if (strcmp(r.type, "disk") == 0 && !r.fstype[0]) continue; /* partitioned disk: its parts speak for it */
        if (strcmp(r.type, "part") != 0 && strcmp(r.type, "disk") != 0) continue; /* skip loop/rom/etc rows */
        StorageRow *copy = g_new(StorageRow, 1);
        *copy = r;
        g_ptr_array_add(rows, copy);
    }
    pclose(f);
    return rows;
}

/* ---- actions --------------------------------------------------------------- */

static void rebuild(void);

static void on_mount_toggle(GtkWidget *btn, gpointer data)
{
    (void)btn;
    StorageRow *r = data;
    char dq[96], cmd[256];
    shell_quote(r->name, dq, sizeof(dq));
    if (r->mountpoint[0])
        snprintf(cmd, sizeof(cmd), "udisksctl unmount -b /dev/%s", dq);
    else
        snprintf(cmd, sizeof(cmd), "udisksctl mount -b /dev/%s", dq);
    run_detached(cmd);
    rebuild();
}

/* Unmounts every mounted partition under `disk_name`, then powers the
 * device off (USB bus power removed -- the udisks2 way to make a stick
 * safe to physically unplug), by shelling a single sh -c script rather
 * than one run_detached() per step: unmount must finish before
 * power-off runs, and run_detached() itself doesn't wait. */
static void on_eject_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    const char *disk_name = data;
    char dq[96], cmd[512];
    shell_quote(disk_name, dq, sizeof(dq));
    snprintf(cmd, sizeof(cmd),
             "for p in /dev/%s?*; do udisksctl unmount -b \"$p\" 2>/dev/null; done; "
             "udisksctl power-off -b /dev/%s",
             dq, dq);
    run_detached(cmd);
    rebuild();
}

/* ---- widget building -------------------------------------------------------- */

static void style_fg(GtkWidget *w)
{
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    GdkColor c = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    static const GtkStateType states[] = {GTK_STATE_NORMAL, GTK_STATE_ACTIVE, GTK_STATE_PRELIGHT, GTK_STATE_SELECTED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        gtk_widget_modify_fg(w, states[i], &c);
        gtk_widget_modify_text(w, states[i], &c);
    }
}

static GtkWidget *build_row(StorageRow *r)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 1);
    char header_markup[256];
    snprintf(header_markup, sizeof(header_markup), "<b>%s</b> \xc2\xb7 <small>%s%s%s</small>",
             r->label[0] ? r->label : r->name, r->size,
             r->fstype[0] ? " \xc2\xb7 " : "", r->fstype);
    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), header_markup);
    gtk_misc_set_alignment(GTK_MISC(header), 0.0f, 0.5f);
    style_fg(header);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    GtkWidget *sub = gtk_label_new(r->mountpoint[0] ? r->mountpoint : "N\xc3\xa3o montado");
    gtk_misc_set_alignment(GTK_MISC(sub), 0.0f, 0.5f);
    style_fg(sub);
    gtk_box_pack_start(GTK_BOX(vbox), sub, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    GtkWidget *mount_btn = gtk_button_new_with_label(r->mountpoint[0] ? "Desmontar" : "Montar");
    g_signal_connect(mount_btn, "clicked", G_CALLBACK(on_mount_toggle), r);
    gtk_box_pack_start(GTK_BOX(hbox), mount_btn, FALSE, FALSE, 0);

    return hbox;
}

static void clear_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_box));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

/* Groups partition rows under their parent disk name (or their own name,
 * for an unpartitioned disk) so one "Remover com seguranca" button can
 * cover every partition of the same physical device. g_free()'d disk
 * names owned by this array; the GPtrArray of StorageRow* it maps to is
 * borrowed from `rows` and not freed here. */
static GHashTable *group_by_disk(GPtrArray *rows)
{
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < rows->len; i++) {
        StorageRow *r = g_ptr_array_index(rows, i);
        const char *disk = r->pkname[0] ? r->pkname : r->name;
        GPtrArray *group = g_hash_table_lookup(groups, disk);
        if (!group) {
            group = g_ptr_array_new();
            g_hash_table_insert(groups, g_strdup(disk), group);
        }
        g_ptr_array_add(group, r);
    }
    return groups;
}

static void rebuild(void)
{
    clear_rows();

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Armazenamento remov\xc3\xadvel</b>");
    gtk_misc_set_alignment(GTK_MISC(title), 0.0f, 0.5f);
    style_fg(title);
    gtk_box_pack_start(GTK_BOX(g_box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 2);

    GPtrArray *rows = fetch_storage();
    if (rows->len == 0) {
        GtkWidget *empty = gtk_label_new("Nenhum dispositivo remov\xc3\xadvel conectado.");
        gtk_misc_set_alignment(GTK_MISC(empty), 0.0f, 0.5f);
        style_fg(empty);
        gtk_box_pack_start(GTK_BOX(g_box), empty, FALSE, FALSE, 0);
    } else {
        GHashTable *groups = group_by_disk(rows);
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, groups);
        gboolean first_group = TRUE;
        while (g_hash_table_iter_next(&it, &key, &value)) {
            const char *disk = key;
            GPtrArray *group = value;
            if (!first_group) gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
            first_group = FALSE;
            for (guint i = 0; i < group->len; i++) {
                gtk_box_pack_start(GTK_BOX(g_box), build_row(g_ptr_array_index(group, i)), FALSE, FALSE, 0);
            }
            GtkWidget *eject_btn = gtk_button_new_with_label("Remover com seguran\xc3\xa7""a");
            g_signal_connect(eject_btn, "clicked", G_CALLBACK(on_eject_clicked), g_strdup(disk));
            /* g_strdup'd disk name is never freed -- these buttons live for the daemon's
             * whole session (rebuilt, not leaked-per-poll: the old row and its closure are
             * destroyed by clear_rows() before this runs again, taking this one string with it). */
            gtk_box_pack_start(GTK_BOX(g_box), eject_btn, FALSE, FALSE, 0);
        }
        g_hash_table_destroy(groups);
    }
    g_ptr_array_free(rows, TRUE);

    gtk_widget_show_all(g_box);
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    rebuild();
    return TRUE;
}

/* ---- page interface ---------------------------------------------------------- */

GtkWidget *page_storage_build(void)
{
    g_box = gtk_vbox_new(FALSE, 4);

    g_root = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_root), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_root), g_box);

    GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(g_root));
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
    double r, g, b, a;
    xisserve_get_bg_rgba(&r, &g, &b, &a);
    GdkColor bg = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    gtk_widget_modify_bg(viewport, GTK_STATE_NORMAL, &bg);

    return g_root;
}

void page_storage_on_show(void)
{
    rebuild();
    if (!g_poll_id) {
        g_poll_id = g_timeout_add(STORAGE_POLL_MS, on_poll, NULL);
    }
}

void page_storage_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}
