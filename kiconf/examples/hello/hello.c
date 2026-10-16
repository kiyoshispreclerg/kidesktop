/* Example kiconf tab module: build with `make`, copy hello.desktop and
 * hello.so to ~/.local/share/kiconf/tabs/. */
#include <gtk/gtk.h>

GtkWidget *kiconf_tab_build(void)
{
    return gtk_label_new("Hello from a plugin tab");
}
