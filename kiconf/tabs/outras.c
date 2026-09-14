/* kiconf - Outras tab: DisablePrimarySelection, DPMS/screensaver, desktop count.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Outras tab widgets + baselines */
static GtkWidget *g_disable_primsel_chk;
static GtkWidget *g_dpms_enabled_chk, *g_dpms_standby_spin, *g_dpms_suspend_spin, *g_dpms_off_spin;
static GtkWidget *g_saver_timeout_spin, *g_saver_cycle_spin, *g_prefer_blank_chk;
static GtkWidget *g_desktop_count_spin;

typedef struct {
    int dpms_enabled, dpms_standby, dpms_suspend, dpms_off;
    int saver_timeout, saver_cycle, prefer_blanking;
} PowerState;
static PowerState g_power_baseline;
static int g_disable_primsel_baseline;
static int g_desktop_count_baseline;

/* ---- Outras tab: 3rd XiS flag + DPMS/screensaver + virtual desktops --- */

static int detect_disable_primsel(void)
{
    char *argv[] = {"xprop", "-root", "_DisablePrimarySelection", NULL};
    char out[256];
    if (!run_capture(argv, out, sizeof(out))) {
        return 0;
    }
    char *eq = strchr(out, '=');
    return eq ? atoi(eq + 1) != 0 : 0;
}

static void set_disable_primsel(int enable)
{
    char *argv[] = {"xprop", "-root", "-f", "_DisablePrimarySelection", "8i",
                     "-set", "_DisablePrimarySelection", enable ? "1" : "0", NULL};
    run_fire(argv);
}

static void detect_power_xset(PowerState *p)
{
    p->dpms_enabled = 1;
    p->dpms_standby = 0;
    p->dpms_suspend = 0;
    p->dpms_off = 0;
    p->saver_timeout = 0;
    p->saver_cycle = 600;
    p->prefer_blanking = 1;
    char *argv[] = {"xset", "q", NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    char *p2;
    if ((p2 = strstr(out, "Standby:"))) {
        sscanf(p2, "Standby:%d Suspend:%d Off:%d", &p->dpms_standby, &p->dpms_suspend, &p->dpms_off);
    }
    p->dpms_enabled = strstr(out, "DPMS is Enabled") != NULL;
    if ((p2 = strstr(out, "timeout:"))) {
        sscanf(p2, "timeout:%d cycle:%d", &p->saver_timeout, &p->saver_cycle);
    }
    if ((p2 = strstr(out, "prefer blanking:"))) {
        p2 += strlen("prefer blanking:");
        while (*p2 == ' ') {
            p2++;
        }
        p->prefer_blanking = strncmp(p2, "yes", 3) == 0;
    }
}

static void apply_power_diff(const PowerState *cur, const PowerState *base)
{
    if (cur->dpms_enabled != base->dpms_enabled) {
        char *argv[] = {"xset", cur->dpms_enabled ? "+dpms" : "-dpms", NULL};
        run_fire(argv);
    }
    if (cur->dpms_standby != base->dpms_standby || cur->dpms_suspend != base->dpms_suspend ||
        cur->dpms_off != base->dpms_off) {
        char a[16], b[16], c[16];
        snprintf(a, sizeof(a), "%d", cur->dpms_standby);
        snprintf(b, sizeof(b), "%d", cur->dpms_suspend);
        snprintf(c, sizeof(c), "%d", cur->dpms_off);
        char *argv[] = {"xset", "dpms", a, b, c, NULL};
        run_fire(argv);
    }
    if (cur->saver_timeout != base->saver_timeout || cur->saver_cycle != base->saver_cycle) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d", cur->saver_timeout);
        snprintf(b, sizeof(b), "%d", cur->saver_cycle);
        char *argv[] = {"xset", "s", a, b, NULL};
        run_fire(argv);
    }
    if (cur->prefer_blanking != base->prefer_blanking) {
        char *argv[] = {"xset", "s", cur->prefer_blanking ? "blank" : "noblank", NULL};
        run_fire(argv);
    }
}

static int detect_desktop_count(void)
{
    char *argv[] = {"xprop", "-root", "_NET_NUMBER_OF_DESKTOPS", NULL};
    char out[256];
    if (!run_capture(argv, out, sizeof(out))) {
        return 1;
    }
    char *eq = strchr(out, '=');
    if (!eq) {
        return 1;
    }
    int v = atoi(eq + 1);
    return v > 0 ? v : 1;
}

static void set_desktop_count(int n)
{
    char nbuf[16];
    snprintf(nbuf, sizeof(nbuf), "%d", n);
    char *argv[] = {"wmctrl", "-n", nbuf, NULL};
    run_fire(argv);
}

static void apply_outras_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    int primsel = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_disable_primsel_chk));
    if (primsel != g_disable_primsel_baseline) {
        set_disable_primsel(primsel);
        g_disable_primsel_baseline = primsel;
    }

    PowerState pcur;
    pcur.dpms_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_dpms_enabled_chk));
    pcur.dpms_standby = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dpms_standby_spin));
    pcur.dpms_suspend = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dpms_suspend_spin));
    pcur.dpms_off = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dpms_off_spin));
    pcur.saver_timeout = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_saver_timeout_spin));
    pcur.saver_cycle = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_saver_cycle_spin));
    pcur.prefer_blanking = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_prefer_blank_chk));
    apply_power_diff(&pcur, &g_power_baseline);
    g_power_baseline = pcur;

    int dcount = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_desktop_count_spin));
    if (dcount != g_desktop_count_baseline) {
        set_desktop_count(dcount);
        g_desktop_count_baseline = dcount;
    }
}

GtkWidget *build_outras_tab(void)
{
    g_disable_primsel_baseline = detect_disable_primsel();
    detect_power_xset(&g_power_baseline);
    g_desktop_count_baseline = detect_desktop_count();

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *primsel_box = gtk_vbox_new(FALSE, 2);
    g_disable_primsel_chk = gtk_check_button_new_with_label(
        "DisablePrimarySelection -- desativa colar com o botao do meio");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_disable_primsel_chk), g_disable_primsel_baseline);
    gtk_box_pack_start(GTK_BOX(primsel_box), g_disable_primsel_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Selecao primaria (XiS)", primsel_box), FALSE, FALSE, 0);

    GtkWidget *dpms_table = gtk_table_new(4, 2, FALSE);
    g_dpms_enabled_chk = gtk_check_button_new_with_label("DPMS habilitado");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_dpms_enabled_chk), g_power_baseline.dpms_enabled);
    gtk_table_attach(GTK_TABLE(dpms_table), g_dpms_enabled_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 2);
    g_dpms_standby_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dpms_standby_spin), g_power_baseline.dpms_standby);
    labeled_row(dpms_table, 1, "Standby, s (0=off):", g_dpms_standby_spin);
    g_dpms_suspend_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dpms_suspend_spin), g_power_baseline.dpms_suspend);
    labeled_row(dpms_table, 2, "Suspend, s (0=off):", g_dpms_suspend_spin);
    g_dpms_off_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dpms_off_spin), g_power_baseline.dpms_off);
    labeled_row(dpms_table, 3, "Off, s (0=off):", g_dpms_off_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("DPMS", dpms_table), FALSE, FALSE, 0);

    GtkWidget *saver_table = gtk_table_new(3, 2, FALSE);
    g_saver_timeout_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_saver_timeout_spin), g_power_baseline.saver_timeout);
    labeled_row(saver_table, 0, "Timeout, s (0=off):", g_saver_timeout_spin);
    g_saver_cycle_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_saver_cycle_spin), g_power_baseline.saver_cycle);
    labeled_row(saver_table, 1, "Ciclo, s:", g_saver_cycle_spin);
    g_prefer_blank_chk = gtk_check_button_new_with_label("Preferir apagar a tela (blank)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_prefer_blank_chk), g_power_baseline.prefer_blanking);
    gtk_table_attach(GTK_TABLE(saver_table), g_prefer_blank_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 2);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Protetor de tela", saver_table), FALSE, FALSE, 0);

    GtkWidget *desk_table = gtk_table_new(1, 2, FALSE);
    g_desktop_count_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_desktop_count_spin), g_desktop_count_baseline);
    labeled_row(desk_table, 0, "Numero de areas de trabalho:", g_desktop_count_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Areas de trabalho virtuais", desk_table), FALSE, FALSE, 0);

    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(apply_outras_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}
