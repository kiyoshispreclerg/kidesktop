/* kiconf - "Sobre" module (system info, fastfetch/hardinfo-style) (fastfetch/hardinfo-style summary).
 * See kiconf.c's top doc comment for the overall design. */
#ifndef KICONF_SYSINFO_H
#define KICONF_SYSINFO_H

#include <gtk/gtk.h>

/* One-shot: reads /etc/os-release, uname(), /proc/{cpuinfo,meminfo},
 * lspci/sysfs and xdpyinfo once and returns a static labeled-facts panel. No
 * live refresh -- none of this changes over the life of a kiconf window,
 * same reasoning as make_output_combo() not re-polling every frame. */
GtkWidget *build_sobre_tab(void);

#endif /* KICONF_SYSINFO_H */
