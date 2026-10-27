/* kiconf - Discos tab: which disks kimountd mounts by itself.
 *
 * kimountd (kisession service) lists in ~/.config/ki-mount.conf every
 * filesystem it has seen that isn't the system's own: removable media,
 * and internal disks that fstab doesn't mount. This tab shows that list
 * with a checkbox per filesystem; a checked one is mounted whenever it
 * shows up, including when it is already connected at login. Aplicar
 * rewrites only the `auto` column (re-reading the file first, so a disk
 * kimountd added meanwhile isn't lost) and sends kimountd a SIGHUP, which
 * also mounts a newly checked disk that is connected right now.
 *
 * Internal disks need polkit's org.freedesktop.udisks2.filesystem-mount-
 * system, which asks for an administrator password by default. The
 * checkbox below the list installs (via pkexec) a rule that grants it to
 * active local users of the sudo/wheel/admin groups, and removes it when
 * unchecked: a .pkla file for polkit 0.105 (Ubuntu 22.04, Debian 11) or a
 * JavaScript .rules file for later polkit (Debian 12+, Devuan 5+). Its
 * state is read back with pkcheck, which answers for whichever of the two
 * the system actually uses -- and for rules that aren't ours: Ubuntu's
 * policykit-desktop-privileges already grants this action to sudo users,
 * so there the box starts checked and unchecking it changes nothing. */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_safefile.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MOUNT_CONF "ki-mount.conf"
#define MOUNT_ACTION "org.freedesktop.udisks2.filesystem-mount-system"

enum { COL_AUTO = 0, COL_NAME, COL_DISK, COL_KIND, COL_FS, COL_SEEN, COL_UUID, N_COLS };

static GtkListStore *g_store;
static GtkWidget *g_view, *g_status, *g_nopasswd_chk;
static GHashTable *g_forgotten; /* uuids removed with Esquecer, until Aplicar */
static int g_nopasswd_busy;      /* ignore the toggle while we set it ourselves */

/* Splits the next tab-separated field off *p (in place). */
static char *tab_field(char **p)
{
    char *s = *p;
    if (!s) {
        return (char *)"";
    }
    char *t = strchr(s, '\t');
    if (t) {
        *t = 0;
        *p = t + 1;
    } else {
        *p = NULL;
    }
    return s;
}

static void load_list(void)
{
    gtk_list_store_clear(g_store);
    char path[PATH_MAX];
    resolve_path(MOUNT_CONF, path, sizeof(path));
    FILE *f = xis_fopen_read(path, xis_check_lines);
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, "DEVICE\t", 7) != 0) {
            continue;
        }
        char *p = line + 7;
        const char *uuid = tab_field(&p);
        int automount = atoi(tab_field(&p)) != 0;
        int removable = atoi(tab_field(&p)) != 0;
        time_t seen = (time_t)atol(tab_field(&p));
        const char *fs = tab_field(&p);
        const char *label = tab_field(&p);
        const char *desc = tab_field(&p);
        if (!uuid[0]) {
            continue;
        }
        char when[32] = "";
        struct tm tm;
        if (seen > 0 && localtime_r(&seen, &tm)) {
            strftime(when, sizeof(when), "%d/%m/%Y %H:%M", &tm);
        }
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it, COL_AUTO, automount, COL_NAME, label[0] ? label : uuid, COL_DISK, desc,
                           COL_KIND, removable ? _("Removivel") : _("Interno"), COL_FS, fs, COL_SEEN, when,
                           COL_UUID, uuid, -1);
    }
    fclose(f);
}

/* The checkbox column's value for uuid, or -1 if it isn't in the list. */
static int auto_of(const char *uuid)
{
    GtkTreeModel *m = GTK_TREE_MODEL(g_store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok; ok = gtk_tree_model_iter_next(m, &it)) {
        gchar *u;
        gboolean a;
        gtk_tree_model_get(m, &it, COL_UUID, &u, COL_AUTO, &a, -1);
        int hit = u && !strcmp(u, uuid);
        g_free(u);
        if (hit) {
            return a ? 1 : 0;
        }
    }
    return -1;
}

static void apply_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char path[PATH_MAX];
    resolve_path(MOUNT_CONF, path, sizeof(path));
    size_t len = 0;
    char *text = xis_read_file_or_backup(path, xis_check_lines, &len);
    if (!text) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Nenhum disco registrado ainda"));
        return;
    }
    char tmp[PATH_MAX + 16];
    xis_write_tmpname(path, tmp, sizeof(tmp));
    FILE *f = xis_write_open(tmp, path);
    if (!f) {
        free(text);
        gtk_label_set_text(GTK_LABEL(g_status), _("Nao foi possivel salvar ki-mount.conf"));
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "DEVICE\t", 7) != 0) {
            fprintf(f, "%s\n", line);
            continue;
        }
        char *uuid = line + 7;
        char *rest = strchr(uuid, '\t');
        if (!rest) {
            continue;
        }
        *rest++ = 0;
        if (g_hash_table_lookup(g_forgotten, uuid)) {
            continue;
        }
        int a = auto_of(uuid);
        char *after = strchr(rest, '\t');
        if (a < 0 || !after) {
            fprintf(f, "DEVICE\t%s\t%s\n", uuid, rest); /* seen after this tab was built: as is */
        } else {
            fprintf(f, "DEVICE\t%s\t%d%s\n", uuid, a, after);
        }
    }
    free(text);
    if (xis_write_commit(f, tmp, path, xis_check_lines) != 0) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Nao foi possivel salvar ki-mount.conf"));
        return;
    }
    g_hash_table_remove_all(g_forgotten);
    signal_daemon("kimountd");
    gtk_label_set_text(GTK_LABEL(g_status), process_running("kimountd")
                                                ? _("Aplicado")
                                                : _("Aplicado, mas o servico kimountd nao esta rodando (aba Servicos)"));
}

static void toggled_cb(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    (void)data;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(g_store), &it, path_str)) {
        return;
    }
    gboolean a;
    gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_AUTO, &a, -1);
    gtk_list_store_set(g_store, &it, COL_AUTO, !a, -1);
}

static void forget_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view));
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        return;
    }
    gchar *uuid;
    gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_UUID, &uuid, -1);
    g_hash_table_insert(g_forgotten, uuid, GINT_TO_POINTER(1)); /* takes uuid */
    gtk_list_store_remove(g_store, &it);
}

/* 1 if this session may mount internal disks without a password now. */
static int nopasswd_active(void)
{
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    gchar *argv[] = {(gchar *)"pkcheck", (gchar *)"--action-id", (gchar *)MOUNT_ACTION, (gchar *)"--process", pid,
                     NULL};
    gint status = 1;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, NULL)) {
        return 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* polkit before 0.106 only reads .pkla files; from 0.106 on, .rules. */
static int polkit_uses_pkla(void)
{
    char out[128];
    char *argv[] = {(char *)"pkaction", (char *)"--version", NULL};
    if (!run_capture(argv, out, sizeof(out))) {
        return 0;
    }
    const char *v = strrchr(trim(out), ' ');
    v = v ? v + 1 : out;
    int major = 0, minor = 0;
    if (sscanf(v, "%d.%d", &major, &minor) < 1) {
        return 0;
    }
    return major == 0 && minor < 106;
}

/* $1 = pkla | rules | remove. Group list: sudo (Debian/Ubuntu/Devuan),
 * admin (old Ubuntu), wheel (Fedora/Arch). */
static const char NOPASSWD_SCRIPT[] =
    "set -e\n"
    "PKLA=/etc/polkit-1/localauthority/50-local.d/50-kidesktop-mount-internal.pkla\n"
    "RULES=/etc/polkit-1/rules.d/50-kidesktop-mount-internal.rules\n"
    "case \"$1\" in\n"
    "pkla)\n"
    "  mkdir -p \"$(dirname \"$PKLA\")\"\n"
    "  cat > \"$PKLA\" <<'EOF'\n"
    "# Installed by KiDesktop (kiconf, Discos tab): mount internal disks without a password.\n"
    "[KiDesktop mount internal disks]\n"
    "Identity=unix-group:sudo;unix-group:admin;unix-group:wheel\n"
    "Action=" MOUNT_ACTION "\n"
    "ResultAny=no\n"
    "ResultInactive=no\n"
    "ResultActive=yes\n"
    "EOF\n"
    "  chmod 644 \"$PKLA\" ;;\n"
    "rules)\n"
    "  mkdir -p \"$(dirname \"$RULES\")\"\n"
    "  cat > \"$RULES\" <<'EOF'\n"
    "// Installed by KiDesktop (kiconf, Discos tab): mount internal disks without a password.\n"
    "polkit.addRule(function(action, subject) {\n"
    "    if (action.id == \"" MOUNT_ACTION "\" && subject.local && subject.active &&\n"
    "        (subject.isInGroup(\"sudo\") || subject.isInGroup(\"admin\") || subject.isInGroup(\"wheel\"))) {\n"
    "        return polkit.Result.YES;\n"
    "    }\n"
    "});\n"
    "EOF\n"
    "  chmod 644 \"$RULES\" ;;\n"
    "remove)\n"
    "  [ -e \"$PKLA\" ] || [ -e \"$RULES\" ] || exit 3\n"
    "  rm -f \"$PKLA\" \"$RULES\" ;;\n"
    "*) exit 2 ;;\n"
    "esac\n";

static void nopasswd_toggled_cb(GtkToggleButton *b, gpointer data)
{
    (void)data;
    if (g_nopasswd_busy) {
        return;
    }
    int want = gtk_toggle_button_get_active(b);
    char *args[] = {(char *)(want ? (polkit_uses_pkla() ? "pkla" : "rules") : "remove"), NULL};
    int rc = run_privileged_script(NOPASSWD_SCRIPT, args, NULL);
    /* polkit re-reads both kinds of file by itself; give it a moment. */
    g_usleep(500 * 1000);
    int now = nopasswd_active();
    g_nopasswd_busy = 1;
    gtk_toggle_button_set_active(b, now);
    g_nopasswd_busy = 0;
    if (!want && rc == 3) {
        /* Ubuntu's policykit-desktop-privileges grants it to sudo users itself. */
        gtk_label_set_text(GTK_LABEL(g_status), _("Esta permissao vem de uma regra do sistema, nao do KiDesktop"));
    } else if (rc != 0) {
        gtk_label_set_text(GTK_LABEL(g_status), _("A alteracao da permissao foi cancelada ou falhou"));
    } else if (now != want) {
        gtk_label_set_text(GTK_LABEL(g_status), want ? _("Regra instalada, mas o polkit ainda pede senha nesta sessao")
                                                     : _("Regra removida (a senha digitada ha pouco ainda vale por alguns minutos)"));
    } else {
        gtk_label_set_text(GTK_LABEL(g_status), want ? _("Discos internos montam sem pedir senha")
                                                     : _("Discos internos voltam a pedir senha"));
    }
}

GtkWidget *build_disks_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *note = gtk_label_new(_("Discos e dispositivos removiveis que ja estiveram conectados a este computador. "
                                      "Os marcados sao montados automaticamente sempre que forem conectados, inclusive "
                                      "se ja estiverem conectados ao iniciar a sessao. A montagem e feita pelo udisks, "
                                      "no mesmo lugar que o KDE e o GNOME usam (em geral /media/USUARIO/NOME). Os discos "
                                      "do sistema e os montados pelo /etc/fstab nao aparecem aqui."));
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    if (g_forgotten) {
        g_hash_table_destroy(g_forgotten); /* from this tab's previous build */
    }
    g_forgotten = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_store = gtk_list_store_new(N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING);
    load_list();
    g_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    g_object_unref(g_store);
    a11y_name(g_view, _("Discos conhecidos"));

    GtkCellRenderer *tog = gtk_cell_renderer_toggle_new();
    g_signal_connect(tog, "toggled", G_CALLBACK(toggled_cb), NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_view),
                                gtk_tree_view_column_new_with_attributes(_("Montar"), tog, "active", COL_AUTO, NULL));
    struct {
        const char *title;
        int col;
    } cols[] = {
        {_("Nome"), COL_NAME},       {_("Disco"), COL_DISK}, {_("Tipo"), COL_KIND},
        {_("Sistema de arquivos"), COL_FS}, {_("Visto por ultimo"), COL_SEEN},
    };
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(cols[i].title, gtk_cell_renderer_text_new(),
                                                                         "text", cols[i].col, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_view), c);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), g_view);
    gtk_widget_set_size_request(sw, -1, 220);
    gtk_box_pack_start(GTK_BOX(outer), sw, TRUE, TRUE, 0);

    g_nopasswd_chk = gtk_check_button_new_with_label(_("Montar discos internos sem pedir senha de administrador"));
    gtk_widget_set_tooltip_text(g_nopasswd_chk,
                                _("Instala uma regra do polkit que libera montar discos internos para usuarios "
                                  "administradores (grupo sudo) na sessao ativa. Pede a senha uma vez para instalar."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_nopasswd_chk), nopasswd_active());
    g_signal_connect(g_nopasswd_chk, "toggled", G_CALLBACK(nopasswd_toggled_cb), NULL);
    gtk_box_pack_start(GTK_BOX(outer), g_nopasswd_chk, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_hbox_new(FALSE, 6);
    g_status = gtk_label_new("");
    GtkWidget *forget = gtk_button_new_with_label(_("Esquecer"));
    gtk_widget_set_tooltip_text(forget, _("Tira o disco selecionado da lista; ele volta se for conectado de novo."));
    g_signal_connect(forget, "clicked", G_CALLBACK(forget_cb), NULL);
    GtkWidget *apply = gtk_button_new_with_label(_("Aplicar"));
    g_signal_connect(apply, "clicked", G_CALLBACK(apply_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btns), forget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), g_status, FALSE, FALSE, 6);
    gtk_box_pack_end(GTK_BOX(btns), apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btns, FALSE, FALSE, 0);
    return outer;
}
