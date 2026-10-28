/* kiconf - Usuarios tab: list, add, edit and remove local user accounts.
 *
 * Listing needs no privileges (getpwent/getgrent). Changes do not run
 * as they are made: each one is queued in the "Alteracoes pendentes"
 * list, and "Aplicar" runs the whole queue as root under a SINGLE
 * pkexec prompt (common.c's run_privileged_script()), so the password is
 * asked once, only when something is actually applied. useradd/usermod/
 * userdel/gpasswd/chpasswd are the same on Ubuntu and Devuan, so there
 * is no init-system branch here.
 *
 * Passwords are hashed here (crypt(3), SHA-512) and handed to
 * `chpasswd -e` on stdin: neither the plain password nor its hash ever
 * appears in a process's argument list. Shadow state (locked or not)
 * is root-only, so an account's lock status is not shown, only set.
 */
#include "../common.h"
#include "../tabs.h"

#include <crypt.h>
#include <ctype.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MIN_UID 1000
#define MAX_UID 60000
#define MAX_OPS 48
#define OP_ARGS 5

enum { COL_NAME, COL_UID, COL_FULL, COL_SHELL, COL_ADMIN, N_COLS };

/* Run as root; the ops arrive as "$@", secrets on stdin. A group that
 * does not exist is skipped silently (sudo vs wheel differs by distro). */
static const char APPLY_SCRIPT[] =
    "set -e\n"
    "while [ $# -gt 0 ]; do\n"
    "  case \"$1\" in\n"
    "    add) useradd -m -c \"$3\" -s \"$4\" -- \"$2\"; shift 4;;\n"
    "    del) userdel -- \"$2\"; shift 2;;\n"
    "    delhome) userdel -r -- \"$2\"; shift 2;;\n"
    "    gecos) usermod -c \"$3\" -- \"$2\"; shift 3;;\n"
    "    shell) usermod -s \"$3\" -- \"$2\"; shift 3;;\n"
    "    lock) usermod -L -- \"$2\"; shift 2;;\n"
    "    unlock) usermod -U -- \"$2\"; shift 2;;\n"
    "    gadd) if getent group \"$3\" >/dev/null; then usermod -aG \"$3\" -- \"$2\"; fi; shift 3;;\n"
    "    gdel) if getent group \"$3\" >/dev/null; then gpasswd -d \"$2\" \"$3\" >/dev/null; fi; shift 3;;\n"
    "    passwd) shift 1;;\n"
    "    *) exit 2;;\n"
    "  esac\n"
    "done\n"
    "chpasswd -e\n";

typedef struct {
    char desc[200];
    char *argv[OP_ARGS + 1];
    int argc;
} Op;

static Op g_ops[MAX_OPS];
static int g_nops;
static char g_hashes[4096]; /* "name:hash\n" lines for chpasswd -e */

static GtkWidget *g_view, *g_ops_view, *g_status;
static GtkListStore *g_store, *g_ops_store;

static int valid_username(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 31 || !(islower((unsigned char)s[0]) || s[0] == '_')) {
        return 0;
    }
    for (; *s; s++) {
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-') {
            return 0;
        }
    }
    return 1;
}

/* Free text that ends up in /etc/passwd's GECOS field. */
static int valid_gecos(const char *s)
{
    for (; *s; s++) {
        if (*s == ':' || *s == '\n' || *s == '\r' || *s == ',') {
            return 0;
        }
    }
    return 1;
}

static const char *admin_group(void)
{
    if (getgrnam("sudo")) {
        return "sudo";
    }
    if (getgrnam("wheel")) {
        return "wheel";
    }
    return NULL;
}

static int user_in_group(const char *user, const char *group)
{
    struct group *g = getgrnam(group);
    if (!g) {
        return 0;
    }
    for (char **m = g->gr_mem; m && *m; m++) {
        if (!strcmp(*m, user)) {
            return 1;
        }
    }
    return 0;
}

static void queue_op(const char *desc, int argc, const char *a0, const char *a1, const char *a2, const char *a3)
{
    if (g_nops >= MAX_OPS) {
        return;
    }
    Op *op = &g_ops[g_nops++];
    snprintf(op->desc, sizeof(op->desc), "%s", desc);
    const char *a[4] = { a0, a1, a2, a3 };
    op->argc = argc;
    for (int i = 0; i < argc && i < 4; i++) {
        op->argv[i] = g_strdup(a[i]);
    }
    op->argv[argc] = NULL;
    GtkTreeIter it;
    gtk_list_store_append(g_ops_store, &it);
    gtk_list_store_set(g_ops_store, &it, 0, desc, -1);
}

static void clear_ops(void)
{
    for (int i = 0; i < g_nops; i++) {
        for (int j = 0; j < g_ops[i].argc; j++) {
            g_free(g_ops[i].argv[j]);
        }
    }
    g_nops = 0;
    g_hashes[0] = '\0';
    gtk_list_store_clear(g_ops_store);
}

/* Queues "set this user's password": the hash waits in g_hashes. */
static int queue_password(const char *user, const char *password)
{
    static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    char salt[32] = "$6$";
    unsigned char rnd[16];
    FILE *f = fopen("/dev/urandom", "r");
    if (!f || fread(rnd, 1, sizeof(rnd), f) != sizeof(rnd)) {
        if (f) {
            fclose(f);
        }
        return 0;
    }
    fclose(f);
    for (int i = 0; i < 16; i++) {
        salt[3 + i] = alphabet[rnd[i] & 63];
    }
    salt[19] = '\0';
    char *hash = crypt(password, salt);
    if (!hash || hash[0] == '*') {
        return 0;
    }
    size_t used = strlen(g_hashes);
    int n = snprintf(g_hashes + used, sizeof(g_hashes) - used, "%s:%s\n", user, hash);
    if (n < 0 || (size_t)n >= sizeof(g_hashes) - used) {
        g_hashes[used] = '\0';
        return 0;
    }
    char desc[200];
    snprintf(desc, sizeof(desc), _("Definir nova senha de %s"), user);
    queue_op(desc, 1, "passwd", NULL, NULL, NULL);
    return 1;
}

static void load_users(void)
{
    gtk_list_store_clear(g_store);
    const char *adm = admin_group();
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent())) {
        if (pw->pw_uid < MIN_UID || pw->pw_uid >= MAX_UID) {
            continue;
        }
        char full[128];
        snprintf(full, sizeof(full), "%s", pw->pw_gecos ? pw->pw_gecos : "");
        char *comma = strchr(full, ',');
        if (comma) {
            *comma = '\0';
        }
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it, COL_NAME, pw->pw_name, COL_UID, (int)pw->pw_uid,
                           COL_FULL, full, COL_SHELL, pw->pw_shell,
                           COL_ADMIN, adm && user_in_group(pw->pw_name, adm) ? _("Sim") : _("Nao"), -1);
    }
    endpwent();
}

static void set_status(const char *msg)
{
    gtk_label_set_text(GTK_LABEL(g_status), msg);
}

static gboolean selected_user(char *name, size_t sz)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view));
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        set_status(_("Selecione um usuario na lista."));
        return FALSE;
    }
    char *n;
    gtk_tree_model_get(model, &it, COL_NAME, &n, -1);
    snprintf(name, sz, "%s", n);
    g_free(n);
    return TRUE;
}

/* The /etc/shells entries that exist, for the shell combo. */
static int load_shells(char shells[16][64])
{
    int n = 0;
    FILE *f = fopen("/etc/shells", "r");
    if (!f) {
        snprintf(shells[n++], 64, "/bin/bash");
        return n;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && n < 16) {
        char *l = trim(line);
        if (*l == '/' && strlen(l) < 64 && access(l, X_OK) == 0 && !strstr(l, "nologin") && !strstr(l, "false")) {
            snprintf(shells[n++], 64, "%s", l);
        }
    }
    fclose(f);
    return n;
}

typedef struct {
    GtkWidget *name, *full, *shell, *admin, *account, *pass, *pass2;
} UserForm;

/* Builds the add/edit form into `dlg`; `edit_user` NULL = new user. */
static UserForm build_form(GtkWidget *dlg, const char *edit_user, const char (*shells)[64], int nshells,
                           const char *cur_full, const char *cur_shell, int cur_admin)
{
    UserForm f;
    GtkWidget *table = gtk_table_new(7, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 8);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);

    f.name = gtk_entry_new();
    if (edit_user) {
        gtk_entry_set_text(GTK_ENTRY(f.name), edit_user);
        gtk_widget_set_sensitive(f.name, FALSE);
    }
    labeled_row(table, 0, _("Usuario:"), f.name);

    f.full = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(f.full), cur_full);
    labeled_row(table, 1, _("Nome completo:"), f.full);

    f.shell = gtk_combo_box_new_text();
    int active = 0;
    for (int i = 0; i < nshells; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(f.shell), shells[i]);
        if (!strcmp(shells[i], cur_shell)) {
            active = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(f.shell), active);
    labeled_row(table, 2, _("Shell:"), f.shell);

    f.admin = gtk_check_button_new_with_label(_("Administrador (pode usar sudo)"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f.admin), cur_admin);
    gtk_table_attach(GTK_TABLE(table), f.admin, 0, 2, 3, 4, GTK_FILL, GTK_FILL, 0, 0);

    f.account = NULL;
    if (edit_user) {
        f.account = gtk_combo_box_new_text();
        gtk_combo_box_append_text(GTK_COMBO_BOX(f.account), _("Manter como esta"));
        gtk_combo_box_append_text(GTK_COMBO_BOX(f.account), _("Bloquear conta"));
        gtk_combo_box_append_text(GTK_COMBO_BOX(f.account), _("Desbloquear conta"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(f.account), 0);
        labeled_row(table, 4, _("Conta:"), f.account);
    }

    f.pass = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(f.pass), FALSE);
    labeled_row(table, 5, edit_user ? _("Nova senha (vazio = manter):") : _("Senha:"), f.pass);
    f.pass2 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(f.pass2), FALSE);
    labeled_row(table, 6, _("Repetir senha:"), f.pass2);

    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), table, TRUE, TRUE, 0);
    gtk_widget_show_all(table);
    return f;
}

static int check_passwords(UserForm *f, int required, GtkWidget *dlg)
{
    const char *p1 = gtk_entry_get_text(GTK_ENTRY(f->pass)), *p2 = gtk_entry_get_text(GTK_ENTRY(f->pass2));
    const char *err = NULL;
    if (strcmp(p1, p2) != 0) {
        err = _("As senhas nao conferem.");
    } else if (required && !*p1) {
        err = _("Informe uma senha.");
    }
    if (err) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                              GTK_BUTTONS_NONE, "%s", err);
        dialog_add_answers(m, GTK_BUTTONS_OK);
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        return 0;
    }
    return 1;
}

static void on_add(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char shells[16][64];
    int nshells = load_shells(shells);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Adicionar usuario"),
        GTK_WINDOW(gtk_widget_get_toplevel(g_view)), GTK_DIALOG_MODAL,
        _("_Cancelar"), GTK_RESPONSE_CANCEL, _("_Adicionar"), GTK_RESPONSE_ACCEPT, NULL);
    UserForm f = build_form(dlg, NULL, shells, nshells, "", "/bin/bash", 0);

    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(f.name));
        const char *full = gtk_entry_get_text(GTK_ENTRY(f.full));
        int si = gtk_combo_box_get_active(GTK_COMBO_BOX(f.shell));
        const char *err = NULL;
        if (!valid_username(name)) {
            err = _("Nome de usuario invalido: use letras minusculas, numeros, '_' e '-', comecando por letra.");
        } else if (getpwnam(name)) {
            err = _("Ja existe um usuario com esse nome.");
        } else if (!valid_gecos(full)) {
            err = _("O nome completo nao pode ter ':' nem ','.");
        } else if (si < 0) {
            err = _("Nenhum shell disponivel.");
        }
        if (err) {
            GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                                  GTK_BUTTONS_NONE, "%s", err);
            dialog_add_answers(m, GTK_BUTTONS_OK);
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            continue;
        }
        if (!check_passwords(&f, 1, dlg)) {
            continue;
        }
        char desc[200];
        snprintf(desc, sizeof(desc), _("Criar usuario %s"), name);
        queue_op(desc, 4, "add", name, full, shells[si]);
        queue_password(name, gtk_entry_get_text(GTK_ENTRY(f.pass)));
        const char *adm = admin_group();
        if (adm && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f.admin))) {
            snprintf(desc, sizeof(desc), _("Tornar %s administrador"), name);
            queue_op(desc, 3, "gadd", name, adm, NULL);
        }
        set_status(_("Alteracao na fila. Clique em Aplicar para executar."));
        break;
    }
    gtk_widget_destroy(dlg);
}

static void on_edit(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char user[64];
    if (!selected_user(user, sizeof(user))) {
        return;
    }
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        return;
    }
    char full[128];
    snprintf(full, sizeof(full), "%s", pw->pw_gecos ? pw->pw_gecos : "");
    char *comma = strchr(full, ',');
    if (comma) {
        *comma = '\0';
    }
    const char *adm = admin_group();
    int was_admin = adm && user_in_group(user, adm);

    char shells[16][64];
    int nshells = load_shells(shells);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Editar usuario"),
        GTK_WINDOW(gtk_widget_get_toplevel(g_view)), GTK_DIALOG_MODAL,
        _("_Cancelar"), GTK_RESPONSE_CANCEL, _("_OK"), GTK_RESPONSE_ACCEPT, NULL);
    UserForm f = build_form(dlg, user, shells, nshells, full, pw->pw_shell, was_admin);

    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *new_full = gtk_entry_get_text(GTK_ENTRY(f.full));
        if (!valid_gecos(new_full)) {
            GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                GTK_BUTTONS_NONE, "%s", _("O nome completo nao pode ter ':' nem ','."));
            dialog_add_answers(m, GTK_BUTTONS_OK);
            gtk_dialog_run(GTK_DIALOG(m));
            gtk_widget_destroy(m);
            continue;
        }
        if (!check_passwords(&f, 0, dlg)) {
            continue;
        }
        char desc[200];
        if (strcmp(new_full, full) != 0) {
            snprintf(desc, sizeof(desc), _("Mudar nome completo de %s"), user);
            queue_op(desc, 3, "gecos", user, new_full, NULL);
        }
        int si = gtk_combo_box_get_active(GTK_COMBO_BOX(f.shell));
        if (si >= 0 && strcmp(shells[si], pw->pw_shell) != 0) {
            snprintf(desc, sizeof(desc), _("Mudar shell de %s para %s"), user, shells[si]);
            queue_op(desc, 3, "shell", user, shells[si], NULL);
        }
        int now_admin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f.admin));
        if (adm && now_admin != was_admin) {
            snprintf(desc, sizeof(desc), now_admin ? _("Tornar %s administrador") : _("Tirar %s de administrador"), user);
            queue_op(desc, 3, now_admin ? "gadd" : "gdel", user, adm, NULL);
        }
        int acc = gtk_combo_box_get_active(GTK_COMBO_BOX(f.account));
        if (acc == 1 || acc == 2) {
            snprintf(desc, sizeof(desc), acc == 1 ? _("Bloquear a conta de %s") : _("Desbloquear a conta de %s"), user);
            queue_op(desc, 2, acc == 1 ? "lock" : "unlock", user, NULL, NULL);
        }
        const char *pw1 = gtk_entry_get_text(GTK_ENTRY(f.pass));
        if (*pw1) {
            queue_password(user, pw1);
        }
        set_status(_("Alteracao na fila. Clique em Aplicar para executar."));
        break;
    }
    gtk_widget_destroy(dlg);
}

static void on_remove(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char user[64];
    if (!selected_user(user, sizeof(user))) {
        return;
    }
    struct passwd *pw = getpwnam(user);
    if (!pw || pw->pw_uid == 0 || pw->pw_uid == getuid()) {
        set_status(_("Nao e possivel remover o root nem o usuario desta sessao."));
        return;
    }
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(g_view)), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Remover o usuario %s?"), user);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), _("_Cancelar"), GTK_RESPONSE_CANCEL, _("_Remover"), GTK_RESPONSE_ACCEPT, NULL);
    dialog_add_answers(dlg, GTK_BUTTONS_NONE); /* just the title */
    GtkWidget *chk = gtk_check_button_new_with_label(_("Apagar tambem a pasta pessoal e os arquivos"));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), chk, FALSE, FALSE, 4);
    gtk_widget_show(chk);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        int home = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk));
        char desc[200];
        snprintf(desc, sizeof(desc), home ? _("Remover %s e a pasta pessoal") : _("Remover %s (mantendo a pasta pessoal)"), user);
        queue_op(desc, 2, home ? "delhome" : "del", user, NULL, NULL);
        set_status(_("Alteracao na fila. Clique em Aplicar para executar."));
    }
    gtk_widget_destroy(dlg);
}

static void on_discard(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    clear_ops();
    set_status(_("Fila descartada."));
}

static void on_apply(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    if (g_nops == 0) {
        set_status(_("Nada para aplicar."));
        return;
    }
    char *args[MAX_OPS * OP_ARGS + 1];
    int n = 0;
    for (int i = 0; i < g_nops; i++) {
        for (int j = 0; j < g_ops[i].argc; j++) {
            args[n++] = g_ops[i].argv[j];
        }
    }
    args[n] = NULL;
    int rc = run_privileged_script(APPLY_SCRIPT, args, g_hashes);
    if (rc == 0) {
        clear_ops();
        set_status(_("Alteracoes aplicadas."));
    } else if (rc == 126 || rc == 127) {
        set_status(_("Autorizacao negada ou cancelada. A fila foi mantida."));
    } else {
        set_status(_("Falha ao aplicar (alguma operacao nao pode ser executada). A fila foi mantida."));
    }
    load_users();
}

GtkWidget *build_users_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    GtkWidget *info = gtk_label_new(_(
        "Contas de usuario do sistema. As alteracoes ficam numa fila e so sao executadas "
        "ao clicar em Aplicar, quando a senha de administrador e pedida uma unica vez."));
    gtk_misc_set_alignment(GTK_MISC(info), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), info, FALSE, FALSE, 0);

    g_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    kiconf_track_ignore(g_view); /* the system's accounts; g_ops_view holds what is pending */
    a11y_name(g_view, _("Usuarios"));
    static const char *const heads[] = { N_("Usuario"), N_("UID"), N_("Nome completo"), N_("Shell"), N_("Administrador") };
    for (int i = 0; i < N_COLS; i++) {
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(_(heads[i]), gtk_cell_renderer_text_new(), "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_view), c);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 180);
    gtk_container_add(GTK_CONTAINER(scroll), g_view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    static const struct { const char *label; GCallback cb; } BTNS[] = {
        { N_("Adicionar..."), G_CALLBACK(on_add) },
        { N_("Editar..."), G_CALLBACK(on_edit) },
        { N_("Remover..."), G_CALLBACK(on_remove) },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *b = gtk_button_new_with_label(_(BTNS[i].label));
        g_signal_connect(b, "clicked", BTNS[i].cb, NULL);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    g_ops_store = gtk_list_store_new(1, G_TYPE_STRING);
    g_ops_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_ops_store));
    a11y_name(g_ops_view, _("Alteracoes pendentes"));
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_ops_view),
        gtk_tree_view_column_new_with_attributes(_("Alteracoes pendentes"), gtk_cell_renderer_text_new(), "text", 0, NULL));
    GtkWidget *oscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(oscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(oscroll, -1, 100);
    gtk_container_add(GTK_CONTAINER(oscroll), g_ops_view);
    gtk_box_pack_start(GTK_BOX(outer), oscroll, FALSE, TRUE, 0);

    GtkWidget *row2 = gtk_hbox_new(FALSE, 6);
    GtkWidget *discard = gtk_button_new_with_label(_("Descartar fila"));
    GtkWidget *apply = gtk_button_new_with_label(_("Aplicar"));
    g_signal_connect(discard, "clicked", G_CALLBACK(on_discard), NULL);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), NULL);
    gtk_box_pack_start(GTK_BOX(row2), discard, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row2), apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row2, FALSE, FALSE, 0);

    g_status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_status), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), g_status, FALSE, FALSE, 0);

    clear_ops();
    load_users();
    return outer;
}
