/* kiterm.h - shared declarations between the kiterm source files. */
#ifndef KITERM_H
#define KITERM_H

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <vte/vte.h>
#include "kt_config.h"

#define KITERM_VERSION "0.1.2"

typedef struct KtWindow {
    GtkWidget *win;
    GtkWidget *notebook;
    GtkWidget *search_bar;      /* kt_search.c */
    GtkWidget *search_entry;
    GtkWidget *search_case, *search_regex;
    KtConfig cfg;
    char *shell_argv0;          /* argv for new tabs: NULL = login shell */
    char **cmd_argv;            /* -e command for the first tab, or NULL */
    char *title_override;       /* --title */
    gboolean argb;              /* window got an RGBA colormap */
    double zoom;                /* font scale, 1.0 = configured size */
    gboolean focused;           /* the window has the input focus */
    gboolean shrinkable;        /* initial size fixed: terminals may request less */
    guint poll_id;              /* background-job poll (kiterm.c), 0 = stopped */
} KtWindow;

/* kiterm.c */
VteTerminal *kt_window_current(KtWindow *w);
void kt_window_new_tab(KtWindow *w, const char *cwd, char **argv);
void kt_window_close_tab(KtWindow *w, GtkWidget *term);
void kt_window_apply_config(KtWindow *w);
void kt_window_update_title(KtWindow *w);
void kt_window_zoom(KtWindow *w, int dir);   /* +1, -1, 0 = reset */
void kt_window_spawn_self(const char *cwd);

/* kt_tab.c */
GtkWidget *kt_tab_create(KtWindow *w, const char *cwd, char **argv);
GtkWidget *kt_tab_make_label(KtWindow *w, VteTerminal *t);
void kt_tab_apply_config(KtWindow *w, VteTerminal *t);
void kt_tab_allow_shrink(VteTerminal *t);
char *kt_tab_cwd(VteTerminal *t);
gboolean kt_tab_busy(VteTerminal *t);
const char *kt_tab_title(VteTerminal *t);
void kt_tab_set_attention(VteTerminal *t, gboolean on);
void kt_tab_job_done(KtWindow *w, VteTerminal *t);
gint64 kt_tab_age_us(VteTerminal *t);
void kt_window_poll_update(KtWindow *w);
void kt_tab_rename(KtWindow *w, VteTerminal *t);
void kt_tab_popup_menu(KtWindow *w, VteTerminal *t, GdkEventButton *ev);

/* kt_search.c */
void kt_search_init(KtWindow *w, GtkWidget *vbox);
void kt_search_show(KtWindow *w);
void kt_search_hide(KtWindow *w);
void kt_search_step(KtWindow *w, gboolean forward);

/* kt_prefs.c */
void kt_prefs_show(KtWindow *w);

#endif
