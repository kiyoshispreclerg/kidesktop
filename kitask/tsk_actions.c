/* tsk_actions.c - what the process menu does: signals, priority, switching
 * to a window, and the details window.
 *
 * Signals and priority changes go straight to the kernel; when that fails
 * with EPERM (someone else's process, or raising priority), kitask offers to
 * retry through pkexec with the kill/renice commands. */
#include "tsk.h"
#include "../shared/xis_i18n.h"
#include "../shared/xis_rootexec.h"
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static const struct {
    int sig;
    const char *name;
} signals[] = {
    {SIGHUP, "SIGHUP"},   {SIGINT, "SIGINT"},   {SIGQUIT, "SIGQUIT"}, {SIGABRT, "SIGABRT"},
    {SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"}, {SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"},
    {SIGKILL, "SIGKILL"}, {SIGSTOP, "SIGSTOP"}, {SIGCONT, "SIGCONT"}, {SIGWINCH, "SIGWINCH"},
};

static const char *proc_name(TskApp *app, pid_t pid)
{
    TskProc *p = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(pid));
    return p ? p->name : "?";
}

static GtkWidget *message_dialog(GtkWindow *parent, GtkDialogFlags flags, GtkMessageType type,
                                 GtkButtonsType buttons, const char *fmt, const char *text)
{
    GtkWidget *d = gtk_message_dialog_new(parent, flags, type, buttons, fmt, text);
    gtk_window_set_title(GTK_WINDOW(d), "kitask");
    return d;
}

static gboolean confirm(TskApp *app, GList *pids, int sig)
{
    const char *what = sig == SIGKILL ? _("Kill") : sig == SIGTERM ? _("End") : sig == SIGSTOP ? _("Pause") : NULL;
    if (!what || !app->cfg.confirm_signals)
        return TRUE;
    guint n = g_list_length(pids);
    char *msg;
    pid_t first = GPOINTER_TO_INT(pids->data);
    if (n == 1) {
        msg = sig == SIGKILL ? g_strdup_printf(_("Kill \"%s\" (PID %d)?"), proc_name(app, first), (int)first)
              : sig == SIGTERM ? g_strdup_printf(_("End \"%s\" (PID %d)?"), proc_name(app, first), (int)first)
                               : g_strdup_printf(_("Pause \"%s\" (PID %d)?"), proc_name(app, first), (int)first);
    } else {
        msg = sig == SIGKILL ? g_strdup_printf(ngettext("Kill %u process?", "Kill %u processes?", n), n)
              : sig == SIGTERM ? g_strdup_printf(ngettext("End %u process?", "End %u processes?", n), n)
                               : g_strdup_printf(ngettext("Pause %u process?", "Pause %u processes?", n), n);
    }
    GtkWidget *d = message_dialog(GTK_WINDOW(app->win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", msg);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(d), "%s",
        sig == SIGKILL ? _("The process stops at once and can't save anything.")
        : sig == SIGTERM ? _("The process is asked to quit; most programs save and exit.")
                         : _("The process stops running until it gets Continue."));
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, what, GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    gboolean ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    g_free(msg);
    return ok;
}

static void error_dialog(TskApp *app, const char *primary, const char *secondary)
{
    GtkWidget *d = message_dialog(GTK_WINDOW(app->win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* TRUE when the user wants to retry through pkexec */
static gboolean ask_admin(TskApp *app, const char *primary)
{
    if (!xis_rootexec_available()) {
        error_dialog(app, primary, _("You don't have permission, and pkexec isn't installed to ask for it."));
        return FALSE;
    }
    GtkWidget *d = message_dialog(GTK_WINDOW(app->win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", primary);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
                                             _("You don't have permission. Try again as administrator?"));
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("Try as _Administrator"),
                           GTK_RESPONSE_ACCEPT, NULL);
    gboolean ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    return ok;
}

static void run_admin(GPtrArray *argv)
{
    g_ptr_array_add(argv, NULL);
    xis_rootexec((char **)argv->pdata, NULL, 0);
}

static const char *signal_name(int sig)
{
    for (size_t i = 0; i < G_N_ELEMENTS(signals); i++)
        if (signals[i].sig == sig)
            return signals[i].name;
    return "SIGTERM";
}

void tsk_send_signal(TskApp *app, int sig)
{
    GList *pids = tsk_list_selected_pids(app);
    if (!pids || !confirm(app, pids, sig)) {
        g_list_free(pids);
        return;
    }
    GPtrArray *denied = g_ptr_array_new_with_free_func(g_free);
    GString *errors = g_string_new(NULL);
    for (GList *l = pids; l; l = l->next) {
        pid_t pid = GPOINTER_TO_INT(l->data);
        if (kill(pid, sig) == 0)
            continue;
        if (errno == EPERM)
            g_ptr_array_add(denied, g_strdup_printf("%d", (int)pid));
        else if (errno != ESRCH)   /* already gone: nothing to tell */
            g_string_append_printf(errors, "%s (%d): %s\n", proc_name(app, pid), (int)pid, g_strerror(errno));
    }
    if (errors->len)
        error_dialog(app, _("Couldn't send the signal"), errors->str);
    if (denied->len) {
        char *msg = g_strdup_printf(_("Couldn't send %s"), signal_name(sig));
        if (ask_admin(app, msg)) {
            GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(argv, g_strdup("kill"));
            g_ptr_array_add(argv, g_strdup_printf("-%d", sig));
            for (guint i = 0; i < denied->len; i++)
                g_ptr_array_add(argv, g_strdup(denied->pdata[i]));
            run_admin(argv);
            g_ptr_array_free(argv, TRUE);
        }
        g_free(msg);
    }
    g_string_free(errors, TRUE);
    g_ptr_array_free(denied, TRUE);
    g_list_free(pids);
    tsk_app_refresh_now(app);
}

void tsk_renice(TskApp *app)
{
    GList *pids = tsk_list_selected_pids(app);
    if (!pids)
        return;
    pid_t first = GPOINTER_TO_INT(pids->data);
    TskProc *p = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(first));
    GtkWidget *d = gtk_dialog_new_with_buttons(_("Change Priority"), GTK_WINDOW(app->win),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_CANCEL,
                                               GTK_RESPONSE_CANCEL, _("_Change"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget *box = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    char *head = g_list_length(pids) == 1
                     ? g_strdup_printf(_("Nice value of \"%s\" (PID %d):"), proc_name(app, first), (int)first)
                     : g_strdup_printf(_("Nice value of the %u selected processes:"), g_list_length(pids));
    GtkWidget *l = gtk_label_new(head);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    g_free(head);
    GtkWidget *scale = gtk_hscale_new_with_range(-20, 19, 1);
    gtk_scale_set_digits(GTK_SCALE(scale), 0);
    gtk_range_set_value(GTK_RANGE(scale), p ? p->nice : 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), scale);
    GtkWidget *hint = gtk_label_new(_("-20 runs first, 19 yields to everything else. Going below the current "
                                      "value needs administrator rights."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(hint), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), box, TRUE, TRUE, 0);
    gtk_widget_set_size_request(d, 380, -1);
    gtk_widget_show_all(d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        int nice = (int)gtk_range_get_value(GTK_RANGE(scale));
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(argv, g_strdup("renice"));
        g_ptr_array_add(argv, g_strdup_printf("%d", nice));
        g_ptr_array_add(argv, g_strdup("-p"));
        guint base = argv->len;
        for (GList *pl = pids; pl; pl = pl->next) {
            pid_t pid = GPOINTER_TO_INT(pl->data);
            if (setpriority(PRIO_PROCESS, (id_t)pid, nice) < 0 && (errno == EPERM || errno == EACCES))
                g_ptr_array_add(argv, g_strdup_printf("%d", (int)pid));
        }
        gtk_widget_destroy(d);
        d = NULL;
        if (argv->len > base && ask_admin(app, _("Couldn't change the priority")))
            run_admin(argv);
        g_ptr_array_free(argv, TRUE);
        tsk_app_refresh_now(app);
    }
    if (d)
        gtk_widget_destroy(d);
    g_list_free(pids);
}

void tsk_activate_window(TskApp *app)
{
    GList *pids = tsk_list_selected_pids(app);
    if (pids) {
        TskProc *p = g_hash_table_lookup(app->procs.procs, pids->data);
        if (p && p->window)
            tsk_x11_activate(p->window);
    }
    g_list_free(pids);
}

/* ---- menu ------------------------------------------------------------------------ */

static void on_signal_item(GtkMenuItem *mi, TskApp *app)
{
    tsk_send_signal(app, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "sig")));
}

static void on_details_item(GtkMenuItem *mi, TskApp *app)
{
    (void)mi;
    GList *pids = tsk_list_selected_pids(app);
    for (GList *l = pids; l; l = l->next)
        tsk_details(app, GPOINTER_TO_INT(l->data));
    g_list_free(pids);
}

static void on_renice_item(GtkMenuItem *mi, TskApp *app)
{
    (void)mi;
    tsk_renice(app);
}

static void on_window_item(GtkMenuItem *mi, TskApp *app)
{
    (void)mi;
    tsk_activate_window(app);
}

static void on_copy_item(GtkMenuItem *mi, TskApp *app)
{
    (void)mi;
    GList *pids = tsk_list_selected_pids(app);
    GString *s = g_string_new(NULL);
    for (GList *l = pids; l; l = l->next) {
        TskProc *p = g_hash_table_lookup(app->procs.procs, l->data);
        if (p)
            g_string_append_printf(s, "%s%s", s->len ? "\n" : "", p->cmdline);
    }
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s->str, -1);
    g_string_free(s, TRUE);
    g_list_free(pids);
}

static GtkWidget *item(GtkWidget *menu, const char *label, GCallback cb, TskApp *app)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(mi, "activate", cb, app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    return mi;
}

static GtkWidget *signal_item(GtkWidget *menu, const char *label, int sig, TskApp *app)
{
    GtkWidget *mi = item(menu, label, G_CALLBACK(on_signal_item), app);
    g_object_set_data(G_OBJECT(mi), "sig", GINT_TO_POINTER(sig));
    return mi;
}

GtkWidget *tsk_process_menu(TskApp *app)
{
    GtkWidget *menu = gtk_menu_new();
    GList *pids = tsk_list_selected_pids(app);
    TskProc *p = pids ? g_hash_table_lookup(app->procs.procs, pids->data) : NULL;
    item(menu, _("_Details"), G_CALLBACK(on_details_item), app);
    GtkWidget *w = item(menu, _("_Switch to Window"), G_CALLBACK(on_window_item), app);
    gtk_widget_set_sensitive(w, p && p->window && !pids->next);
    item(menu, _("C_opy Command Line"), G_CALLBACK(on_copy_item), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    signal_item(menu, _("_End (SIGTERM)"), SIGTERM, app);
    signal_item(menu, _("_Kill (SIGKILL)"), SIGKILL, app);
    signal_item(menu, _("_Pause (SIGSTOP)"), SIGSTOP, app);
    signal_item(menu, _("Co_ntinue (SIGCONT)"), SIGCONT, app);
    GtkWidget *sub = gtk_menu_new();
    for (size_t i = 0; i < G_N_ELEMENTS(signals); i++) {
        char *label = g_strdup_printf("%d  %s", signals[i].sig, signals[i].name);
        GtkWidget *mi = gtk_menu_item_new_with_label(label);
        g_object_set_data(G_OBJECT(mi), "sig", GINT_TO_POINTER(signals[i].sig));
        g_signal_connect(mi, "activate", G_CALLBACK(on_signal_item), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        g_free(label);
    }
    GtkWidget *more = gtk_menu_item_new_with_mnemonic(_("Send _Signal"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(more), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), more);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    item(menu, _("Change _Priority..."), G_CALLBACK(on_renice_item), app);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    g_list_free(pids);
    return menu;
}

/* ---- details window ---------------------------------------------------------------- */

typedef struct {
    TskApp *app;
    pid_t pid;
    unsigned long long start;
    GtkWidget *win, *table, *ended;
    GHashTable *values;     /* row name -> GtkLabel */
    GtkTextBuffer *env, *files;
    guint timer;
    int rows;
} Details;

static GtkWidget *value_label(Details *d, const char *key, const char *title)
{
    GtkWidget *k = gtk_label_new(title);
    gtk_misc_set_alignment(GTK_MISC(k), 1, 0);
    GtkWidget *v = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(v), 0, 0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_table_attach(GTK_TABLE(d->table), k, 0, 1, (guint)d->rows, (guint)d->rows + 1, GTK_FILL, GTK_FILL, 0, 0);
    gtk_table_attach(GTK_TABLE(d->table), v, 1, 2, (guint)d->rows, (guint)d->rows + 1, GTK_FILL | GTK_EXPAND,
                     GTK_FILL, 0, 0);
    d->rows++;
    g_hash_table_insert(d->values, (gpointer)key, v);
    return v;
}

static void section(Details *d, const char *title)
{
    char *markup = g_markup_printf_escaped("<b>%s</b>", title);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 1);
    gtk_misc_set_padding(GTK_MISC(l), 0, d->rows ? 6 : 0);
    gtk_table_attach(GTK_TABLE(d->table), l, 0, 2, (guint)d->rows, (guint)d->rows + 1, GTK_FILL, GTK_FILL, 0, 0);
    d->rows++;
    g_free(markup);
}

static void set_value(Details *d, const char *key, char *text)
{
    GtkWidget *l = g_hash_table_lookup(d->values, key);
    if (l && strcmp(gtk_label_get_text(GTK_LABEL(l)), text ? text : ""))
        gtk_label_set_text(GTK_LABEL(l), text ? text : "");
    g_free(text);
}

static char *bytes_or_dash(double v)
{
    return v < 0 ? g_strdup("—") : tsk_fmt_bytes(v);
}

static char *rate_pair(double total, double rate)
{
    if (total < 0)
        return g_strdup(_("— (not your process)"));
    char *t = tsk_fmt_bytes(total), *r = tsk_fmt_rate(MAX(rate, 0));
    char *s = g_strdup_printf(_("%s, now %s"), t, r);
    g_free(t);
    g_free(r);
    return s;
}

static char *read_link(pid_t pid, const char *name)
{
    char path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, name);
    char *l = g_file_read_link(path, NULL);
    return l ? l : g_strdup(_("— (not readable)"));
}

static gint cmp_int(gconstpointer a, gconstpointer b)
{
    return *(const int *)a - *(const int *)b;
}

static void fill_files(Details *d)
{
    char path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/fd", (int)d->pid);
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        gtk_text_buffer_set_text(d->files, _("Not readable: the process belongs to another user."), -1);
        return;
    }
    GArray *fds = g_array_new(FALSE, FALSE, sizeof(int));
    const char *n;
    while ((n = g_dir_read_name(dir))) {
        int fd = atoi(n);
        g_array_append_val(fds, fd);
    }
    g_dir_close(dir);
    g_array_sort(fds, cmp_int);
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < fds->len; i++) {
        char lp[80];
        int fd = g_array_index(fds, int, i);
        g_snprintf(lp, sizeof(lp), "/proc/%d/fd/%d", (int)d->pid, fd);
        char *target = g_file_read_link(lp, NULL);
        g_string_append_printf(s, "%4d  %s\n", fd, target ? target : "?");
        g_free(target);
    }
    gtk_text_buffer_set_text(d->files, s->str, (int)s->len);
    g_string_free(s, TRUE);
    g_array_free(fds, TRUE);
}

static void fill_env(Details *d)
{
    gsize len = 0;
    char *env = tsk_proc_read_file(d->pid, "environ", &len);
    if (!env) {
        gtk_text_buffer_set_text(d->env, _("Not readable: the process belongs to another user."), -1);
        return;
    }
    for (gsize i = 0; i < len; i++)
        if (env[i] == '\0')
            env[i] = '\n';
    gtk_text_buffer_set_text(d->env, env, (int)len);
    g_free(env);
}

static gboolean details_refresh(gpointer data)
{
    Details *d = data;
    TskApp *app = d->app;
    TskProc *p = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(d->pid));
    if (!p || p->start != d->start) {
        gtk_widget_show(d->ended);
        d->timer = 0;
        return FALSE;
    }
    char *title = g_strdup_printf(_("%s (PID %d) - Details"), p->name, (int)p->pid);
    gtk_window_set_title(GTK_WINDOW(d->win), title);
    g_free(title);
    TskProc *parent = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(p->ppid));
    set_value(d, "name", g_strdup(p->name));
    set_value(d, "pid", g_strdup_printf("%d", (int)p->pid));
    set_value(d, "ppid", parent ? g_strdup_printf("%s (%d)", parent->name, (int)p->ppid)
                                : g_strdup_printf("%d", (int)p->ppid));
    set_value(d, "user", g_strdup_printf("%s (%u)", tsk_user_name(p->uid), (unsigned)p->uid));
    set_value(d, "cmdline", g_strdup(p->cmdline));
    set_value(d, "exe", read_link(p->pid, "exe"));
    set_value(d, "cwd", read_link(p->pid, "cwd"));
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)(app->procs.boot_time + p->start / app->procs.hz));
    set_value(d, "start", g_date_time_format(dt, "%c"));
    g_date_time_unref(dt);
    long t = (long)(p->ticks / app->procs.hz);
    set_value(d, "cpu", g_strdup_printf(_("%.1f%% of all cores, %.1f%% of one; %ld:%02ld:%02ld in total"),
                                        p->cpu / app->procs.ncpu, p->cpu, t / 3600, t / 60 % 60, t % 60));
    set_value(d, "threads", g_strdup_printf(_("%d threads, nice %d, state %c"), p->threads, p->nice, p->state));
    set_value(d, "rss", tsk_fmt_bytes(p->rss));
    set_value(d, "shared", tsk_fmt_bytes(p->shared));
    set_value(d, "vsize", tsk_fmt_bytes(p->vsize));

    /* the main list reads smaps/io only for visible columns: read them here */
    char *roll = tsk_proc_read_file(p->pid, "smaps_rollup", NULL);
    double pss = -1, uss = -1, swap = -1;
    if (roll) {
        const char *s;
        if ((s = strstr(roll, "\nPss:")))
            pss = g_ascii_strtod(s + 5, NULL) * 1024;
        double pc = 0, pd = 0;
        if ((s = strstr(roll, "\nPrivate_Clean:")))
            pc = g_ascii_strtod(s + 15, NULL) * 1024;
        if ((s = strstr(roll, "\nPrivate_Dirty:")))
            pd = g_ascii_strtod(s + 15, NULL) * 1024;
        uss = pc + pd;
        if ((s = strstr(roll, "\nSwap:")))
            swap = g_ascii_strtod(s + 6, NULL) * 1024;
        g_free(roll);
    }
    set_value(d, "uss", bytes_or_dash(uss));
    set_value(d, "pss", bytes_or_dash(pss));
    set_value(d, "swap", bytes_or_dash(swap));
    set_value(d, "x11", p->x11_mem >= 0 ? tsk_fmt_bytes(p->x11_mem) : g_strdup(_("— (column hidden or no X connection)")));
    if (p->gpu_mem >= 0) {
        char *m = tsk_fmt_bytes(p->gpu_mem);
        set_value(d, "gpumem", g_strdup_printf(_("%s, %.0f%% busy"), m, MAX(p->gpu, 0)));
        g_free(m);
    } else {
        set_value(d, "gpumem", g_strdup(_("— (column hidden or not using a GPU)")));
    }
    char *io = tsk_proc_read_file(p->pid, "io", NULL);
    if (io) {
        const char *r = strstr(io, "\nread_bytes:"), *w = strstr(io, "\nwrite_bytes:");
        double rd = r ? g_ascii_strtod(r + 12, NULL) : 0, wr = w ? g_ascii_strtod(w + 13, NULL) : 0;
        set_value(d, "ioread", rate_pair(rd, p->io_read_rate));
        set_value(d, "iowrite", rate_pair(wr, p->io_write_rate));
        g_free(io);
    } else {
        set_value(d, "ioread", rate_pair(-1, 0));
        set_value(d, "iowrite", rate_pair(-1, 0));
    }
    if (p->net_rx_rate >= 0) {
        char *rx = tsk_fmt_rate(p->net_rx_rate), *tx = tsk_fmt_rate(p->net_tx_rate);
        set_value(d, "net", g_strdup_printf(_("in %s, out %s (TCP)"), rx, tx));
        g_free(rx);
        g_free(tx);
    } else {
        set_value(d, "net", g_strdup(_("— (column hidden or not your process)")));
    }
    int nwin = tsk_x11_window_count(p->pid);
    set_value(d, "window", p->title ? (nwin > 1 ? g_strdup_printf(_("%s (and %d more)"), p->title, nwin - 1)
                                                : g_strdup(p->title))
                                    : g_strdup(_("—")));
    return TRUE;
}

static void details_destroy(GtkWidget *w, Details *d)
{
    (void)w;
    if (d->timer)
        g_source_remove(d->timer);
    g_hash_table_destroy(d->values);
    g_free(d);
}

static gboolean details_configure(GtkWidget *w, GdkEventConfigure *ev, Details *d)
{
    (void)w;
    d->app->cfg.details_w = ev->width;
    d->app->cfg.details_h = ev->height;
    tsk_app_save_soon(d->app);
    return FALSE;
}

static GtkWidget *text_page(GtkTextBuffer **buf)
{
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_CHAR);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace");
    gtk_widget_modify_font(tv, fd);
    pango_font_description_free(fd);
    *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    return sw;
}

static void on_details_response(GtkDialog *dlg, int resp, Details *d)
{
    if (resp == 1) {
        fill_env(d);
        fill_files(d);
        details_refresh(d);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

void tsk_details(TskApp *app, pid_t pid)
{
    TskProc *p = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(pid));
    if (!p)
        return;
    Details *d = g_new0(Details, 1);
    d->app = app;
    d->pid = pid;
    d->start = p->start;
    d->values = g_hash_table_new(g_str_hash, g_str_equal);
    d->win = gtk_dialog_new_with_buttons("", GTK_WINDOW(app->win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_STOCK_REFRESH, 1, GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d->win), app->cfg.details_w, app->cfg.details_h);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);

    d->table = gtk_table_new(1, 2, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(d->table), 10);
    gtk_table_set_row_spacings(GTK_TABLE(d->table), 3);
    gtk_container_set_border_width(GTK_CONTAINER(d->table), 10);
    section(d, _("Process"));
    value_label(d, "name", _("Name:"));
    value_label(d, "pid", _("PID:"));
    value_label(d, "ppid", _("Parent:"));
    value_label(d, "user", _("User:"));
    value_label(d, "start", _("Started:"));
    value_label(d, "cpu", _("CPU:"));
    value_label(d, "threads", _("Threads:"));
    value_label(d, "window", _("Window:"));
    value_label(d, "exe", _("Executable:"));
    value_label(d, "cwd", _("Working folder:"));
    value_label(d, "cmdline", _("Command line:"));
    section(d, _("Memory"));
    value_label(d, "uss", _("Exclusive:"));
    value_label(d, "pss", _("Proportional:"));
    value_label(d, "rss", _("Resident:"));
    value_label(d, "shared", _("Shared:"));
    value_label(d, "swap", _("Swap:"));
    value_label(d, "vsize", _("Virtual:"));
    value_label(d, "x11", _("X11 pixmaps:"));
    value_label(d, "gpumem", _("GPU:"));
    section(d, _("Input/Output"));
    value_label(d, "ioread", _("Disk read:"));
    value_label(d, "iowrite", _("Disk written:"));
    value_label(d, "net", _("Network:"));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(sw), d->table);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(gtk_bin_get_child(GTK_BIN(sw))), GTK_SHADOW_NONE);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sw, gtk_label_new(_("General")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), text_page(&d->env), gtk_label_new(_("Environment")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), text_page(&d->files), gtk_label_new(_("Open Files")));

    d->ended = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", _("This process has ended."));
    gtk_label_set_markup(GTK_LABEL(d->ended), markup);
    g_free(markup);
    gtk_widget_set_no_show_all(d->ended, TRUE);
    GtkWidget *vbox = GTK_DIALOG(d->win)->vbox;
    gtk_box_pack_start(GTK_BOX(vbox), d->ended, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), nb, TRUE, TRUE, 0);
    g_signal_connect(d->win, "response", G_CALLBACK(on_details_response), d);
    g_signal_connect(d->win, "destroy", G_CALLBACK(details_destroy), d);
    g_signal_connect(d->win, "configure-event", G_CALLBACK(details_configure), d);
    fill_env(d);
    fill_files(d);
    details_refresh(d);
    d->timer = g_timeout_add((guint)(app->cfg.list_interval * 1000), details_refresh, d);
    gtk_widget_show_all(d->win);
}
