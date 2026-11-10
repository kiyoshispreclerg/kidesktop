/* kiconf - Acoes de arquivos tab: the installable context-menu actions of
 * kifiles (service menus, see kifiles/SERVICEMENUS.md).
 *
 * Empty for now. It will list the actions found in kifiles/actions and
 * kio/servicemenus with a checkbox each, and write the unchecked ones as
 * `disabled=<file>[:<token>]` lines in ki-fileactions.conf, which kifiles
 * already reads at every right-click. */
#include "../common.h"
#include "../tabs.h"

GtkWidget *build_fileactions_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    GtkWidget *hint = gtk_label_new(
        _("As acoes instaladas para o menu de contexto do gerenciador de arquivos aparecerao aqui, "
          "para ligar ou desligar cada uma."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(hint), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), hint, FALSE, FALSE, 0);
    return outer;
}
