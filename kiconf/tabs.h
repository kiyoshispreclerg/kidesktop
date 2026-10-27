/* kiconf - prototypes for the per-tab GtkNotebook page builders.
 * See kiconf.c's top doc comment for what each tab does; each
 * implementation lives in its own file under tabs/. */
#ifndef KICONF_TABS_H
#define KICONF_TABS_H

#include <gtk/gtk.h>

GtkWidget *build_appearance_tab(void);
GtkWidget *build_kitheme_tab(void);
GtkWidget *build_shortcuts_tab(void);
GtkWidget *build_input_tab(void);
GtkWidget *build_permissions_tab(void);
GtkWidget *build_panels_tab(void);
GtkWidget *build_wallpaper_tab(void);
GtkWidget *build_screens_tab(void);
GtkWidget *build_windows_tab(void);
GtkWidget *build_effects_tab(void);
/* Hot corners frame inside the Efeitos tab (tabs/corners.c): built once,
 * and asked for the text of its kicomp.conf sections on every Aplicar. */
GtkWidget *corners_build(void);
void corners_write(FILE *out);
GtkWidget *build_system_tab(void);
GtkWidget *build_energy_tab(void);
GtkWidget *build_programs_tab(void);
GtkWidget *build_associations_tab(void);
GtkWidget *build_menu_tab(void);
GtkWidget *build_autostart_tab(void);
GtkWidget *build_events_tab(void);
GtkWidget *build_history_tab(void);
GtkWidget *build_accessibility_tab(void);
GtkWidget *build_sounds_tab(void);
GtkWidget *build_users_tab(void);
GtkWidget *build_services_tab(void);
GtkWidget *build_login_tab(void);
GtkWidget *build_lock_tab(void);
GtkWidget *build_disks_tab(void);
/* Tab shown only when i3lock and kilock are installed. */
int lock_available(void);

#endif /* KICONF_TABS_H */
