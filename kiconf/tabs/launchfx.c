/* kiconf - launch feedback section of the Efeitos tab.
 *
 * The zoom+fade over the icon of a launched entry (xispanel, kifiles,
 * xisserve) is configured here once for all of them, in launchfx.conf --
 * not in kicomp.conf, which only the compositor reads. Each program keeps
 * its own on/off toggle. Not a tab of its own: launchfx_build() gives the
 * frame and launchfx_write() runs on the Efeitos tab's "Aplicar".
 *
 * First build with no launchfx.conf: the zoom/duration an older xispanel.conf
 * tasklist line carried (launch_feedback_zoom= / launch_feedback_ms=) are
 * adopted, so nobody loses a value they had set. */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_launchfx_conf.h"
#include "../../shared/xis_safefile.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GtkWidget *g_zoom_spin, *g_ms_spin;

/* Looks for launch_feedback_zoom= / launch_feedback_ms= on a tasklist
 * WIDGET line of xispanel.conf. */
static void migrate_from_xispanel(double *zoom, int *ms)
{
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    FILE *f = xis_fopen_read(path, xis_check_usable);
    if (!f)
        return;
    char line[4096];
    int got_zoom = 0, got_ms = 0;
    while (fgets(line, sizeof(line), f) && !(got_zoom && got_ms)) {
        if (strncmp(line, "WIDGET\t", 7) != 0 || !strstr(line, "\ttasklist\t"))
            continue;
        char *p = strstr(line, "launch_feedback_zoom=");
        if (p && !got_zoom) {
            *zoom = atof(p + strlen("launch_feedback_zoom="));
            got_zoom = 1;
        }
        p = strstr(line, "launch_feedback_ms=");
        if (p && !got_ms) {
            *ms = atoi(p + strlen("launch_feedback_ms="));
            got_ms = 1;
        }
    }
    fclose(f);
}

GtkWidget *launchfx_build(void)
{
    double zoom = XIS_LAUNCHFX_ZOOM_DEFAULT;
    int ms = XIS_LAUNCHFX_MS_DEFAULT;
    char path[PATH_MAX];
    xis_launchfx_conf_path(path, sizeof(path));
    if (xis_file_or_backup_exists(path))
        xis_launchfx_conf_get(&zoom, &ms);
    else
        migrate_from_xispanel(&zoom, &ms);
    if (zoom < XIS_LAUNCHFX_ZOOM_MIN)
        zoom = XIS_LAUNCHFX_ZOOM_MIN;
    if (zoom > XIS_LAUNCHFX_ZOOM_MAX)
        zoom = XIS_LAUNCHFX_ZOOM_MAX;
    if (ms < XIS_LAUNCHFX_MS_MIN)
        ms = XIS_LAUNCHFX_MS_MIN;
    if (ms > XIS_LAUNCHFX_MS_MAX)
        ms = XIS_LAUNCHFX_MS_MAX;

    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    g_zoom_spin = gtk_spin_button_new_with_range(XIS_LAUNCHFX_ZOOM_MIN, XIS_LAUNCHFX_ZOOM_MAX, 0.25);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_zoom_spin), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_zoom_spin), zoom);
    labeled_row(table, 0, _("Ampliacao do icone:"), g_zoom_spin);
    gtk_widget_set_tooltip_text(g_zoom_spin, _("Quantas vezes o icone cresce ate o fim da animacao (2.0 = o dobro)"));
    g_ms_spin = gtk_spin_button_new_with_range(XIS_LAUNCHFX_MS_MIN, XIS_LAUNCHFX_MS_MAX, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_ms_spin), ms);
    labeled_row(table, 1, _("Duracao (ms):"), g_ms_spin);
    gtk_widget_set_tooltip_text(g_ms_spin, _("Quanto dura a animacao"));

    GtkWidget *box = gtk_vbox_new(FALSE, 4);
    GtkWidget *hint = gtk_label_new(_("Zoom e fade sobre o icone de um item lancado. Vale para todos os programas "
                                      "(painel, kifiles, xisserve); cada um liga ou desliga o seu efeito nas proprias "
                                      "opcoes. Precisa de compositor. Grava launchfx.conf."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(hint), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), table, FALSE, FALSE, 0);
    return frame_with(_("Feedback ao lancar"), box);
}

/* Returns 0 on success (or when the section was never built). */
int launchfx_write(void)
{
    if (!g_zoom_spin)
        return 0;
    char path[PATH_MAX];
    xis_launchfx_conf_path(path, sizeof(path));
    char tmp[PATH_MAX + 8];
    xis_write_tmpname(path, tmp, sizeof(tmp));
    FILE *out = xis_write_open(tmp, path);
    if (!out) {
        g_warning("kiconf: could not write '%s'", tmp);
        return -1;
    }
    fprintf(out, "# launch feedback (zoom+fade over a launched icon), shared by xispanel/kifiles/xisserve.\n");
    fprintf_double(out, "zoom", gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_zoom_spin)), 2);
    fprintf(out, "duration_ms=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_ms_spin)));
    return xis_write_commit(out, tmp, path, xis_check_lines);
}
