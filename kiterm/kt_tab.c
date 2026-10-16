/* kt_tab.c - one terminal inside the notebook: spawning the shell,
 * per-terminal settings, tab label, URL ctrl+click, drops and the context
 * menu. */
#include "kiterm.h"
#include "../shared/xis_i18n.h"
#include <limits.h>
#include "../shared/xis_sound.h"
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define KEY_PID   "kt-child-pid"
#define KEY_LABEL "kt-tab-label"
#define KEY_CUSTOM "kt-custom-title"
#define KEY_ATTN  "kt-attention"
#define KEY_BORN  "kt-born-us"

static GRegex *url_regex;

static const char *const kUrlPattern =
    "(?:[a-z][a-z0-9+.-]*://|www\\.)[-A-Za-z0-9._~:/?#@!$&'()*+,;=%\\[\\]]*[-A-Za-z0-9_~/#@$&=%+]";

void kt_tab_apply_config(KtWindow *w, VteTerminal *t)
{
    const KtConfig *c = &w->cfg;
    PangoFontDescription *fd = pango_font_description_from_string(c->font);
    if (w->zoom != 1.0) {
        double size = pango_font_description_get_size(fd) * w->zoom;
        pango_font_description_set_size(fd, (gint)(size > PANGO_SCALE ? size : PANGO_SCALE));
    }
    vte_terminal_set_font(t, fd);
    pango_font_description_free(fd);
    vte_terminal_set_colors(t, &c->fg, &c->bg, c->palette, 16);
    vte_terminal_set_color_cursor(t, &c->cursor);
    vte_terminal_set_opacity(t, (guint16)(c->opacity * 65535.0));
    vte_terminal_set_scrollback_lines(t, c->scrollback);
    vte_terminal_set_cursor_shape(t, c->cursor_shape);
    vte_terminal_set_cursor_blink_mode(t, c->cursor_blink ? VTE_CURSOR_BLINK_ON : VTE_CURSOR_BLINK_OFF);
    /* VTE's own beep is always off: the bell goes through the sound theme
     * (on_beep), so one setting controls it for the whole desktop. */
    vte_terminal_set_audible_bell(t, FALSE);
    vte_terminal_set_scroll_on_output(t, c->scroll_on_output);
    vte_terminal_set_scroll_on_keystroke(t, c->scroll_on_keystroke);
    vte_terminal_set_word_chars(t, c->word_chars);
    vte_terminal_set_mouse_autohide(t, TRUE);
}

/* VTE requests the size of its current grid, so a window could only ever
 * grow. Once the initial size is fixed the request drops to a few cells and
 * the window manager's size is what counts. */
void kt_tab_allow_shrink(VteTerminal *t)
{
    glong cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
    gtk_widget_set_size_request(GTK_WIDGET(t), (gint)(cw * 4), (gint)(ch * 2));
}

char *kt_tab_cwd(VteTerminal *t)
{
    GPid pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), KEY_PID));
    if (pid <= 0)
        return NULL;
    char link[64], buf[PATH_MAX];
    g_snprintf(link, sizeof(link), "/proc/%d/cwd", (int)pid);
    ssize_t n = readlink(link, buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = 0;
    return g_strdup(buf);
}

/* A foreground job other than the shell itself is running. */
gboolean kt_tab_busy(VteTerminal *t)
{
    GPid pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), KEY_PID));
    int fd = vte_terminal_get_pty(t);
    if (pid <= 0 || fd < 0)
        return FALSE;
    pid_t fg = tcgetpgrp(fd);
    return fg > 0 && fg != pid;
}

const char *kt_tab_title(VteTerminal *t)
{
    const char *custom = g_object_get_data(G_OBJECT(t), KEY_CUSTOM);
    if (custom)
        return custom;
    const char *title = vte_terminal_get_window_title(t);
    return title && *title ? title : "Terminal";
}

static void tab_label_refresh(VteTerminal *t)
{
    GtkWidget *label = g_object_get_data(G_OBJECT(t), KEY_LABEL);
    if (!label)
        return;
    const char *title = kt_tab_title(t);
    /* A dot marks a tab that rang the bell or finished a job unseen. */
    char *txt = g_object_get_data(G_OBJECT(t), KEY_ATTN) ? g_strconcat("\xe2\x97\x8f ", title, NULL) : g_strdup(title);
    gtk_label_set_text(GTK_LABEL(label), txt);
    g_free(txt);
    gtk_widget_set_tooltip_text(label, title);
}

void kt_tab_set_attention(VteTerminal *t, gboolean on)
{
    if ((g_object_get_data(G_OBJECT(t), KEY_ATTN) != NULL) == on)
        return;
    g_object_set_data(G_OBJECT(t), KEY_ATTN, on ? GINT_TO_POINTER(1) : NULL);
    tab_label_refresh(t);
}

gint64 kt_tab_age_us(VteTerminal *t)
{
    gint64 born = (gint64)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(t), KEY_BORN));
    return g_get_monotonic_time() - born;
}

/* A program that was running in a tab nobody was looking at is done. */
void kt_tab_job_done(KtWindow *w, VteTerminal *t)
{
    xis_sound_play("complete");
    kt_tab_set_attention(t, TRUE);
    if (w->cfg.urgent_on_bell && !w->focused)
        gtk_window_set_urgency_hint(GTK_WINDOW(w->win), TRUE);
}

static void on_title_changed(VteTerminal *t, gpointer data)
{
    KtWindow *w = data;
    tab_label_refresh(t);
    kt_window_update_title(w);
}

static void on_child_exited(VteTerminal *t, gpointer data)
{
    KtWindow *w = data;
    /* A `-e` command ending in a tab or window nobody is watching. */
    if (w->cfg.notify_done && w->cmd_argv && (!w->focused || t != kt_window_current(w)) &&
        kt_tab_age_us(t) >= (gint64)w->cfg.notify_done_secs * G_USEC_PER_SEC)
        kt_tab_job_done(w, t);
    kt_window_close_tab(w, GTK_WIDGET(t));
}

static void on_beep(VteTerminal *t, gpointer data)
{
    KtWindow *w = data;
    if (w->cfg.audible_bell)
        xis_sound_play("bell");
    if (!w->focused || t != kt_window_current(w))
        kt_tab_set_attention(t, TRUE);
    if (w->cfg.urgent_on_bell && !w->focused)
        gtk_window_set_urgency_hint(GTK_WINDOW(w->win), TRUE);
}


static void open_url(const char *url)
{
    char *full = g_str_has_prefix(url, "www.") ? g_strconcat("http://", url, NULL) : g_strdup(url);
    const char *argv[] = { "xdg-open", full, NULL };
    g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
    g_free(full);
}

static gboolean on_button_press(GtkWidget *wd, GdkEventButton *ev, gpointer data)
{
    KtWindow *w = data;
    VteTerminal *t = VTE_TERMINAL(wd);
    if (ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (ev->button == 1 && (ev->state & GDK_CONTROL_MASK)) {
        glong cw = vte_terminal_get_char_width(t), ch = vte_terminal_get_char_height(t);
        if (cw <= 0 || ch <= 0)
            return FALSE;
        int tag;
        char *m = vte_terminal_match_check(t, (glong)(ev->x / cw), (glong)(ev->y / ch), &tag);
        if (m) {
            open_url(m);
            g_free(m);
            return TRUE;
        }
    } else if (ev->button == 3 && !(ev->state & GDK_SHIFT_MASK)) {
        kt_tab_popup_menu(w, t, ev);
        return TRUE;
    }
    return FALSE;
}

/* Ctrl+wheel zooms the font, like Ctrl+Plus / Ctrl+Minus. */
static gboolean on_scroll(GtkWidget *wd, GdkEventScroll *ev, gpointer data)
{
    (void)wd;
    if ((ev->state & gtk_accelerator_get_default_mod_mask()) != GDK_CONTROL_MASK)
        return FALSE;
    if (ev->direction == GDK_SCROLL_UP)
        kt_window_zoom(data, 1);
    else if (ev->direction == GDK_SCROLL_DOWN)
        kt_window_zoom(data, -1);
    return TRUE;
}

/* Dropped files become shell-quoted paths, not the file:// URIs VTE's own
 * handler would type. */
static void on_drag_data(GtkWidget *wd, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel,
                         guint info, guint time, gpointer data)
{
    (void)x; (void)y; (void)info; (void)data;
    GdkAtom type = gtk_selection_data_get_data_type(sel);
    char *tname = gdk_atom_name(type);
    gboolean is_uris = tname && !strcmp(tname, "text/uri-list");
    g_free(tname);
    if (!is_uris)
        return;
    char **uris = gtk_selection_data_get_uris(sel);
    if (uris) {
        GString *out = g_string_new(NULL);
        for (char **u = uris; *u; u++) {
            char *path = g_filename_from_uri(*u, NULL, NULL);
            char *q = g_shell_quote(path ? path : *u);
            g_string_append(out, q);
            g_string_append_c(out, ' ');
            g_free(q);
            g_free(path);
        }
        vte_terminal_feed_child(VTE_TERMINAL(wd), out->str, out->len);
        g_string_free(out, TRUE);
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
    g_signal_stop_emission_by_name(wd, "drag-data-received");
}

static void on_close_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    GtkWidget *term = g_object_get_data(G_OBJECT(b), "kt-term");
    kt_window_close_tab(data, term);
}

GtkWidget *kt_tab_make_label(KtWindow *w, VteTerminal *t)
{
    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    GtkWidget *label = gtk_label_new("Terminal");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);

    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
    gtk_container_add(GTK_CONTAINER(btn), gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(btn, _("Close tab"));
    g_object_set_data(G_OBJECT(btn), "kt-term", t);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_close_clicked), w);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    g_object_set_data(G_OBJECT(t), KEY_LABEL, label);
    return box;
}

GtkWidget *kt_tab_create(KtWindow *w, const char *cwd, char **argv)
{
    if (!url_regex)
        url_regex = g_regex_new(kUrlPattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);

    GtkWidget *wd = vte_terminal_new();
    VteTerminal *t = VTE_TERMINAL(wd);
    kt_tab_apply_config(w, t);
    vte_terminal_set_size(t, w->cfg.cols, w->cfg.rows);
    if (w->shrinkable)
        kt_tab_allow_shrink(t);
    if (url_regex) {
        int tag = vte_terminal_match_add_gregex(t, url_regex, 0);
        vte_terminal_match_set_cursor_type(t, tag, GDK_HAND2);
    }

    g_signal_connect(t, "window-title-changed", G_CALLBACK(on_title_changed), w);
    g_signal_connect(t, "child-exited", G_CALLBACK(on_child_exited), w);
    g_signal_connect(t, "beep", G_CALLBACK(on_beep), w);
    g_signal_connect(t, "button-press-event", G_CALLBACK(on_button_press), w);
    g_signal_connect(t, "scroll-event", G_CALLBACK(on_scroll), w);
    g_signal_connect(t, "drag-data-received", G_CALLBACK(on_drag_data), w);

    /* Command: the -e argv, else the configured shell, else $SHELL. */
    char *shell_argv[2] = { NULL, NULL };
    char **run = argv;
    if (!run) {
        const char *sh = w->cfg.shell[0] ? w->cfg.shell : g_getenv("SHELL");
        shell_argv[0] = (char *)(sh && *sh ? sh : "/bin/sh");
        run = shell_argv;
    }
    GPid pid = 0;
    GError *err = NULL;
    /* Login shells are not wanted (a terminal inherits the session's
     * environment); VTE's PTY flags add the utmp/wtmp entries. */
    if (!vte_terminal_fork_command_full(t, VTE_PTY_DEFAULT, cwd, run, NULL,
                                        G_SPAWN_SEARCH_PATH,
                                        NULL, NULL, &pid, &err)) {
        char *msg = g_strdup_printf(_("Could not start \"%s\": %s"), run[0], err ? err->message : "?");
        vte_terminal_feed(t, msg, -1);
        vte_terminal_feed(t, "\r\n", 2);
        g_free(msg);
        g_clear_error(&err);
    }
    g_object_set_data(G_OBJECT(t), KEY_PID, GINT_TO_POINTER((int)pid));
    g_object_set_data(G_OBJECT(t), KEY_BORN, GSIZE_TO_POINTER((gsize)g_get_monotonic_time()));
    return wd;
}

/* ---- rename + context menu --------------------------------------------- */

void kt_tab_rename(KtWindow *w, VteTerminal *t)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Rename tab"), GTK_WINDOW(w->win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *entry = gtk_entry_new();
    const char *custom = g_object_get_data(G_OBJECT(t), KEY_CUSTOM);
    gtk_entry_set_text(GTK_ENTRY(entry), custom ? custom : kt_tab_title(t));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), entry, TRUE, TRUE, 6);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
        /* An empty name goes back to following the program's own title. */
        g_object_set_data_full(G_OBJECT(t), KEY_CUSTOM, *txt ? g_strdup(txt) : NULL, g_free);
        tab_label_refresh(t);
        kt_window_update_title(w);
    }
    gtk_widget_destroy(dlg);
}

static void m_copy(GtkMenuItem *i, gpointer d) { (void)i; vte_terminal_copy_clipboard(VTE_TERMINAL(d)); }
static void m_paste(GtkMenuItem *i, gpointer d) { (void)i; vte_terminal_paste_clipboard(VTE_TERMINAL(d)); }
static void m_selall(GtkMenuItem *i, gpointer d) { (void)i; vte_terminal_select_all(VTE_TERMINAL(d)); }
static void m_find(GtkMenuItem *i, gpointer d) { (void)i; kt_search_show(d); }
static void m_prefs(GtkMenuItem *i, gpointer d) { (void)i; kt_prefs_show(d); }
static void m_rename(GtkMenuItem *i, gpointer d)
{
    (void)i;
    KtWindow *w = d;
    kt_tab_rename(w, kt_window_current(w));
}
static void m_newtab(GtkMenuItem *i, gpointer d)
{
    (void)i;
    KtWindow *w = d;
    char *cwd = kt_tab_cwd(kt_window_current(w));
    kt_window_new_tab(w, cwd, NULL);
    g_free(cwd);
}
static void m_newwin(GtkMenuItem *i, gpointer d)
{
    (void)i;
    char *cwd = kt_tab_cwd(kt_window_current(d));
    kt_window_spawn_self(cwd);
    g_free(cwd);
}
static void m_close(GtkMenuItem *i, gpointer d)
{
    (void)i;
    KtWindow *w = d;
    kt_window_close_tab(w, GTK_WIDGET(kt_window_current(w)));
}
/* Signals offered in the menu, to the terminal's foreground process group
 * (the running program, or the shell when idle). */
static const struct { int num; const char *name; const char *desc; } kSignals[] = {
    { SIGINT,  "SIGINT",  N_("interrupt") },
    { SIGTERM, "SIGTERM", N_("terminate") },
    { SIGKILL, "SIGKILL", N_("kill") },
    { SIGHUP,  "SIGHUP",  N_("hang up") },
    { SIGQUIT, "SIGQUIT", N_("quit") },
    { SIGSTOP, "SIGSTOP", N_("stop") },
    { SIGTSTP, "SIGTSTP", N_("suspend") },
    { SIGCONT, "SIGCONT", N_("continue") },
    { SIGUSR1, "SIGUSR1", N_("user-defined 1") },
    { SIGUSR2, "SIGUSR2", N_("user-defined 2") },
    { SIGALRM, "SIGALRM", N_("alarm") },
    { SIGWINCH, "SIGWINCH", N_("window size change") },
};

static void m_signal(GtkMenuItem *i, gpointer d)
{
    VteTerminal *t = d;
    int fd = vte_terminal_get_pty(t);
    pid_t pg = fd >= 0 ? tcgetpgrp(fd) : -1;
    if (pg <= 0)
        pg = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), KEY_PID));
    if (pg > 0)
        killpg(pg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(i), "kt-signal")));
}

static GtkWidget *signal_menu(VteTerminal *t)
{
    GtkWidget *sub = gtk_menu_new();
    for (size_t n = 0; n < G_N_ELEMENTS(kSignals); n++) {
        char *label = g_strdup_printf("%s (%d) - %s", kSignals[n].name, kSignals[n].num, _(kSignals[n].desc));
        GtkWidget *mi = gtk_menu_item_new_with_label(label);
        g_free(label);
        g_object_set_data(G_OBJECT(mi), "kt-signal", GINT_TO_POINTER(kSignals[n].num));
        g_signal_connect(mi, "activate", G_CALLBACK(m_signal), t);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
    }
    return sub;
}

static void m_reset(GtkMenuItem *i, gpointer d)
{
    (void)i;
    vte_terminal_reset(VTE_TERMINAL(d), TRUE, TRUE);
}

static void add_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data, gboolean sens)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    gtk_widget_set_sensitive(mi, sens);
    g_signal_connect(mi, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void add_sep(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

void kt_tab_popup_menu(KtWindow *w, VteTerminal *t, GdkEventButton *ev)
{
    GtkWidget *menu = gtk_menu_new();
    add_item(menu, _("_Copy"), G_CALLBACK(m_copy), t, vte_terminal_get_has_selection(t));
    add_item(menu, _("_Paste"), G_CALLBACK(m_paste), t, TRUE);
    add_item(menu, _("Select _all"), G_CALLBACK(m_selall), t, TRUE);
    add_sep(menu);
    add_item(menu, _("_Find..."), G_CALLBACK(m_find), w, TRUE);
    add_sep(menu);
    add_item(menu, _("New _tab"), G_CALLBACK(m_newtab), w, TRUE);
    add_item(menu, _("New _window"), G_CALLBACK(m_newwin), w, TRUE);
    add_item(menu, _("_Rename tab..."), G_CALLBACK(m_rename), w, TRUE);
    add_item(menu, _("C_lose tab"), G_CALLBACK(m_close), w, TRUE);
    add_sep(menu);
    GtkWidget *sig = gtk_menu_item_new_with_mnemonic(_("Send _signal"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(sig), signal_menu(t));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sig);
    add_item(menu, _("Reset terminal"), G_CALLBACK(m_reset), t, TRUE);
    add_item(menu, _("Pre_ferences..."), G_CALLBACK(m_prefs), w, TRUE);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev ? ev->button : 0,
                   ev ? ev->time : gtk_get_current_event_time());
}
