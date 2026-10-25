/* kiconf - Servicos tab: status, start/stop/restart and enable/disable
 * of system services, on systemd, OpenRC or plain sysvinit.
 *
 * The init system is detected once (systemd if /run/systemd/system
 * exists, else OpenRC if rc-service is installed, else sysvinit's
 * `service`). Listing needs no privileges on any of them; a start/stop/
 * restart/enable/disable runs as root through pkexec, which is where
 * the password is asked -- once per action.
 *
 *   systemd  : systemctl list-units + list-unit-files / systemctl <verb>
 *   OpenRC   : rc-status --all + rc-update show / rc-service, rc-update
 *   sysvinit : service --status-all + /etc/rc2.d symlinks /
 *              service, update-rc.d
 *
 * Service names reach the privileged command as a single argument
 * ("$2" of a constant script), after being checked against a strict
 * character set. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SVC 2048
#define LIST_BUF (1024 * 1024)

typedef enum { INIT_SYSTEMD, INIT_OPENRC, INIT_SYSV } InitKind;

typedef struct {
    char name[96];
    char desc[128];
    int running;      /* 1 running, 0 stopped, -1 unknown */
    int enabled;      /* 1 enabled, 0 disabled, -1 unknown/static */
} Svc;

enum { COL_NAME, COL_STATE, COL_ENABLED, COL_DESC, N_COLS };

static InitKind g_init;
static Svc *g_svc;
static int g_nsvc;
static GtkWidget *g_view, *g_filter, *g_status;
static GtkListStore *g_store;
static GtkTreeModel *g_filtered;

/* $1 = verb, $2 = service name. The mapping to the init system's own
 * commands is fixed here; nothing from the UI is interpreted by sh. */
static const char SCRIPT_SYSTEMD[] =
    "case \"$1\" in start|stop|restart|enable|disable) systemctl \"$1\" -- \"$2\";; *) exit 2;; esac";
static const char SCRIPT_OPENRC[] =
    "case \"$1\" in start|stop|restart) rc-service \"$2\" \"$1\";;"
    " enable) rc-update add \"$2\" default;; disable) rc-update del \"$2\" default;; *) exit 2;; esac";
static const char SCRIPT_SYSV[] =
    "case \"$1\" in start|stop|restart) service \"$2\" \"$1\";;"
    " enable|disable) update-rc.d \"$2\" \"$1\";; *) exit 2;; esac";

static int have_cmd(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }
    char dirs[4096];
    snprintf(dirs, sizeof(dirs), "%s:/usr/sbin:/sbin", path);
    char *save = NULL;
    for (char *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (fmt_fits(full, sizeof(full), "%s/%s", d, name) && access(full, X_OK) == 0) {
            return 1;
        }
    }
    return 0;
}

static void detect_init(void)
{
    if (access("/run/systemd/system", F_OK) == 0) {
        g_init = INIT_SYSTEMD;
    } else if (have_cmd("rc-service")) {
        g_init = INIT_OPENRC;
    } else {
        g_init = INIT_SYSV;
    }
}

static int valid_service_name(const char *s)
{
    if (!*s || strlen(s) > 95 || *s == '-') {
        return 0;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr("@._:-", *s)) {
            return 0;
        }
    }
    return 1;
}

static Svc *find_or_add(const char *name)
{
    for (int i = 0; i < g_nsvc; i++) {
        if (!strcmp(g_svc[i].name, name)) {
            return &g_svc[i];
        }
    }
    if (g_nsvc >= MAX_SVC || !valid_service_name(name)) {
        return NULL;
    }
    Svc *s = &g_svc[g_nsvc++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->running = -1;
    s->enabled = -1;
    return s;
}

static void strip_service_suffix(char *name)
{
    size_t n = strlen(name);
    if (n > 8 && !strcmp(name + n - 8, ".service")) {
        name[n - 8] = '\0';
    }
}

/* ---- systemd ------------------------------------------------------------ */

static void list_systemd(char *buf)
{
    char *units[] = {"systemctl", "list-units", "--type=service", "--all", "--plain", "--no-legend", "--no-pager", NULL};
    run_capture(units, buf, LIST_BUF);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char unit[96], load[24], active[24], sub[24];
        int consumed = 0;
        if (sscanf(line, "%95s %23s %23s %23s %n", unit, load, active, sub, &consumed) < 4) {
            continue;
        }
        if (!strcmp(load, "not-found")) {
            continue;
        }
        strip_service_suffix(unit);
        Svc *s = find_or_add(unit);
        if (!s) {
            continue;
        }
        s->running = !strcmp(sub, "running") || !strcmp(sub, "exited") || !strcmp(active, "active");
        snprintf(s->desc, sizeof(s->desc), "%s", line + consumed);
    }
    char *files[] = {"systemctl", "list-unit-files", "--type=service", "--no-legend", "--no-pager", NULL};
    run_capture(files, buf, LIST_BUF);
    save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char unit[96], state[32];
        if (sscanf(line, "%95s %31s", unit, state) < 2) {
            continue;
        }
        strip_service_suffix(unit);
        Svc *s = find_or_add(unit);
        if (!s) {
            continue;
        }
        if (!strcmp(state, "enabled") || !strcmp(state, "enabled-runtime")) {
            s->enabled = 1;
        } else if (!strcmp(state, "disabled")) {
            s->enabled = 0;
        }
    }
}

/* ---- OpenRC ------------------------------------------------------------- */

static void list_openrc(char *buf)
{
    char *status[] = {"rc-status", "--all", NULL};
    run_capture(status, buf, LIST_BUF);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[96];
        if (sscanf(line, " %95s", name) != 1 || name[0] == '[' || !strchr(line, '[')) {
            continue;
        }
        char *b = strchr(line, '[');
        Svc *s = find_or_add(name);
        if (s) {
            s->running = strstr(b, "started") != NULL;
        }
    }
    char *show[] = {"rc-update", "show", "-v", NULL};
    run_capture(show, buf, LIST_BUF);
    save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[96];
        if (sscanf(line, " %95s |", name) != 1) {
            continue;
        }
        Svc *s = find_or_add(name);
        if (s) {
            s->enabled = strchr(line, '|') && strlen(strchr(line, '|')) > 3;
        }
    }
    for (int i = 0; i < g_nsvc; i++) {
        if (g_svc[i].enabled < 0) {
            g_svc[i].enabled = 0;
        }
    }
}

/* ---- sysvinit ----------------------------------------------------------- */

static int sysv_enabled(const char *name)
{
    DIR *d = opendir("/etc/rc2.d");
    if (!d) {
        return -1;
    }
    int found = 0;
    struct dirent *e;
    size_t nl = strlen(name);
    while ((e = readdir(d))) {
        if (e->d_name[0] == 'S' && strlen(e->d_name) == nl + 3 && !strcmp(e->d_name + 3, name)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static void list_sysv(char *buf)
{
    char *argv[] = {"service", "--status-all", NULL};
    char path_fix[PATH_MAX];
    snprintf(path_fix, sizeof(path_fix), "/usr/sbin/service");
    if (access(path_fix, X_OK) == 0) {
        argv[0] = path_fix;
    }
    /* `service --status-all` writes to stderr on some versions; the
     * capture only takes stdout, which is where Debian's prints it. */
    run_capture(argv, buf, LIST_BUF);
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char mark;
        char name[96];
        if (sscanf(line, " [ %c ] %95s", &mark, name) != 2) {
            continue;
        }
        Svc *s = find_or_add(name);
        if (!s) {
            continue;
        }
        s->running = mark == '+' ? 1 : mark == '-' ? 0 : -1;
        s->enabled = sysv_enabled(name);
    }
}

/* ---- list + UI ------------------------------------------------------------ */

static int svc_cmp(const void *a, const void *b)
{
    return strcmp(((const Svc *)a)->name, ((const Svc *)b)->name);
}

static const char *running_text(int v)
{
    return v > 0 ? _("Em execucao") : v == 0 ? _("Parado") : _("?");
}

static const char *enabled_text(int v)
{
    return v > 0 ? _("Ativado") : v == 0 ? _("Desativado") : _("Fixo/?");
}

static void reload_services(void)
{
    g_nsvc = 0;
    char *buf = malloc(LIST_BUF);
    if (!buf) {
        return;
    }
    switch (g_init) {
    case INIT_SYSTEMD: list_systemd(buf); break;
    case INIT_OPENRC:  list_openrc(buf); break;
    case INIT_SYSV:    list_sysv(buf); break;
    }
    free(buf);
    qsort(g_svc, (size_t)g_nsvc, sizeof(Svc), svc_cmp);

    gtk_list_store_clear(g_store);
    for (int i = 0; i < g_nsvc; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it, COL_NAME, g_svc[i].name, COL_STATE, running_text(g_svc[i].running),
                           COL_ENABLED, enabled_text(g_svc[i].enabled), COL_DESC, g_svc[i].desc, -1);
    }
}

static gboolean row_visible(GtkTreeModel *model, GtkTreeIter *it, gpointer data)
{
    (void)data;
    const char *needle = gtk_entry_get_text(GTK_ENTRY(g_filter));
    if (!*needle) {
        return TRUE;
    }
    char *name, *desc;
    gtk_tree_model_get(model, it, COL_NAME, &name, COL_DESC, &desc, -1);
    char *ln = g_ascii_strdown(name ? name : "", -1), *ld = g_ascii_strdown(desc ? desc : "", -1);
    char *nl = g_ascii_strdown(needle, -1);
    gboolean ok = strstr(ln, nl) || strstr(ld, nl);
    g_free(ln);
    g_free(ld);
    g_free(nl);
    g_free(name);
    g_free(desc);
    return ok;
}

static void on_filter_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(g_filtered));
}

static gboolean selected_service(char *name, size_t sz)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view));
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Selecione um servico na lista."));
        return FALSE;
    }
    char *n;
    gtk_tree_model_get(model, &it, COL_NAME, &n, -1);
    snprintf(name, sz, "%s", n);
    g_free(n);
    return TRUE;
}

static void do_action(const char *verb)
{
    char name[96];
    if (!selected_service(name, sizeof(name)) || !valid_service_name(name)) {
        return;
    }
    if (!strcmp(verb, "stop") || !strcmp(verb, "disable")) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(g_view)), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, _("Confirma (%s) o servico %s?"), verb, name);
        int r = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (r != GTK_RESPONSE_YES) {
            return;
        }
    }
    const char *script = g_init == INIT_SYSTEMD ? SCRIPT_SYSTEMD : g_init == INIT_OPENRC ? SCRIPT_OPENRC : SCRIPT_SYSV;
    char *args[] = {(char *)verb, name, NULL};
    int rc = run_privileged_script(script, args, NULL);
    char msg[256];
    if (rc == 0) {
        snprintf(msg, sizeof(msg), _("%s: %s executado."), name, verb);
    } else if (rc == 126 || rc == 127) {
        snprintf(msg, sizeof(msg), "%s", _("Autorizacao negada ou cancelada."));
    } else {
        snprintf(msg, sizeof(msg), _("%s: %s falhou (codigo %d)."), name, verb, rc);
    }
    reload_services();
    gtk_label_set_text(GTK_LABEL(g_status), msg);
}

static void on_start(GtkWidget *w, gpointer d)   { (void)w; (void)d; do_action("start"); }
static void on_stop(GtkWidget *w, gpointer d)    { (void)w; (void)d; do_action("stop"); }
static void on_restart(GtkWidget *w, gpointer d) { (void)w; (void)d; do_action("restart"); }
static void on_enable(GtkWidget *w, gpointer d)  { (void)w; (void)d; do_action("enable"); }
static void on_disable(GtkWidget *w, gpointer d) { (void)w; (void)d; do_action("disable"); }
static void on_refresh(GtkWidget *w, gpointer d) { (void)w; (void)d; reload_services(); gtk_label_set_text(GTK_LABEL(g_status), ""); }

GtkWidget *build_services_tab(void)
{
    detect_init();
    if (!g_svc) {
        g_svc = calloc(MAX_SVC, sizeof(Svc));
    }

    GtkWidget *outer = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    const char *kind = g_init == INIT_SYSTEMD ? "systemd" : g_init == INIT_OPENRC ? "OpenRC" : "sysvinit";
    char text[400];
    snprintf(text, sizeof(text), _("Servicos do sistema (%s). Ver o estado nao exige senha; iniciar, parar, reiniciar, "
                                   "ativar ou desativar pede a senha de administrador."), kind);
    GtkWidget *info = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(info), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), info, FALSE, FALSE, 0);

    GtkWidget *frow = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(frow), gtk_label_new(_("Filtrar:")), FALSE, FALSE, 0);
    g_filter = gtk_entry_new();
    a11y_name(g_filter, _("Filtrar servicos"));
    g_signal_connect(g_filter, "changed", G_CALLBACK(on_filter_changed), NULL);
    gtk_box_pack_start(GTK_BOX(frow), g_filter, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frow, FALSE, FALSE, 0);

    g_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_filtered = gtk_tree_model_filter_new(GTK_TREE_MODEL(g_store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(g_filtered), row_visible, NULL, NULL);
    g_view = gtk_tree_view_new_with_model(g_filtered);
    a11y_name(g_view, _("Servicos"));
    static const char *const heads[] = { N_("Servico"), N_("Estado"), N_("Na inicializacao"), N_("Descricao") };
    for (int i = 0; i < N_COLS; i++) {
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(_(heads[i]), gtk_cell_renderer_text_new(), "text", i, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (i == COL_NAME) {
            gtk_tree_view_column_set_sort_column_id(c, COL_NAME);
        }
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_view), c);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 260);
    gtk_container_add(GTK_CONTAINER(scroll), g_view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    static const struct { const char *label; GCallback cb; } BTNS[] = {
        { N_("Iniciar"), G_CALLBACK(on_start) },
        { N_("Parar"), G_CALLBACK(on_stop) },
        { N_("Reiniciar"), G_CALLBACK(on_restart) },
        { N_("Ativar na inicializacao"), G_CALLBACK(on_enable) },
        { N_("Desativar na inicializacao"), G_CALLBACK(on_disable) },
    };
    for (int i = 0; i < 5; i++) {
        GtkWidget *b = gtk_button_new_with_label(_(BTNS[i].label));
        g_signal_connect(b, "clicked", BTNS[i].cb, NULL);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    }
    GtkWidget *refresh = gtk_button_new_from_stock(GTK_STOCK_REFRESH);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), NULL);
    gtk_box_pack_end(GTK_BOX(row), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    g_status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_status), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), g_status, FALSE, FALSE, 0);

    reload_services();
    return outer;
}
