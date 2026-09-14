/* kiconf - prototypes for the per-tab GtkNotebook page builders.
 * See kiconf.c's top doc comment for what each tab does; each
 * implementation lives in its own file under tabs/. */
#ifndef KICONF_TABS_H
#define KICONF_TABS_H

#include <gtk/gtk.h>

GtkWidget *build_appearance_tab(void);
GtkWidget *build_shortcuts_tab(void);
GtkWidget *build_entrada_tab(void);
GtkWidget *build_outras_tab(void);
GtkWidget *build_permissoes_tab(void);
GtkWidget *build_paineis_tab(void);
GtkWidget *build_wallpaper_tab(void);
GtkWidget *build_telas_tab(void);

#endif /* KICONF_TABS_H */
