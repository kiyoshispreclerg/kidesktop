/*
 * ring.c - `xisserve --ring alarm|timer <id>`: the popup that rings when
 * an alarm or countdown timer from ki-clock.conf (see
 * ../shared/xis_clock.h) comes due. kiconfd is what starts it -- it's the
 * one process always running to notice -- so this rings even when the
 * --calendar page (or the whole xisserve daemon) isn't up.
 *
 * Plays the freedesktop "alarm-clock-elapsed" sound in a loop until the
 * user answers, or for "CLOCK ring_minutes" (xisserve.conf, default 5)
 * at most. Alarms offer "Soneca" ("CLOCK snooze_minutes", default 10),
 * timers "+1 min"; both of those, and "Parar", are written straight back
 * to ki-clock.conf (load, change, save), which wakes kiconfd and any open
 * --calendar page through their own file watching.
 *
 * Same one-shot deal as --session/--question: dispatched from main()
 * before the singleton lock, so it never disturbs the launcher and two
 * things coming due together just get two popups.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

#include "xisserve.h"
#include "../shared/xis_clock.h"

#include <signal.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const kSoundFiles[] = {
    "/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga",
    "/usr/share/sounds/freedesktop/stereo/bell.oga",
};
/* Each takes the file as its only argument and exits when done. */
static const char *const kPlayers[] = {"paplay", "pw-play", "ogg123", "mpv"};

static gboolean g_is_alarm;
static int g_id;
static gboolean g_ringing = TRUE;
static GPid g_player_pid;
static guint g_beep_id;
static char *g_player;
static const char *g_sound;

static void play_once(void);

static gboolean replay_cb(gpointer data)
{
    (void)data;
    play_once();
    return FALSE;
}

static void on_player_exit(GPid pid, gint status, gpointer data)
{
    (void)data;
    g_spawn_close_pid(pid);
    g_player_pid = 0;
    /* A player that fails (no sound server) would otherwise be retried
     * forever: drop to the bell instead. Killed by stop_sound() is fine. */
    if (g_ringing && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        g_free(g_player);
        g_player = NULL;
    }
    if (g_ringing) g_timeout_add(400, replay_cb, NULL);
}

static gboolean beep_cb(gpointer data)
{
    (void)data;
    gdk_beep();
    return g_ringing;
}

static void play_once(void)
{
    if (!g_ringing) return;
    if (g_player && g_sound) {
        char *argv[5];
        int n = 0;
        argv[n++] = g_player;
        if (!strcmp(g_player, "mpv")) {
            argv[n++] = "--no-video"; /* mpv would otherwise open a window */
            argv[n++] = "--really-quiet";
        }
        argv[n++] = (char *)g_sound;
        argv[n] = NULL;
        if (g_spawn_async(NULL, argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL |
                              G_SPAWN_STDERR_TO_DEV_NULL,
                          NULL, NULL, &g_player_pid, NULL)) {
            g_child_watch_add(g_player_pid, on_player_exit, NULL);
            return;
        }
    }
    /* No player or no sound file: the X bell is better than silence. */
    if (!g_beep_id) {
        gdk_beep();
        g_beep_id = g_timeout_add(1500, beep_cb, NULL);
    }
}

static void stop_sound(void)
{
    g_ringing = FALSE;
    if (g_player_pid) kill(g_player_pid, SIGTERM);
    if (g_beep_id) {
        g_source_remove(g_beep_id);
        g_beep_id = 0;
    }
}

typedef enum { ANSWER_STOP, ANSWER_SNOOZE, ANSWER_PLUS_MINUTE } Answer;

static void apply_answer(Answer answer)
{
    XisClock c;
    xis_clock_load(&c);
    if (g_is_alarm) {
        XisAlarm *a = xis_clock_find_alarm(&c, g_id);
        if (!a) return;
        if (answer == ANSWER_SNOOZE) {
            int minutes = xisserve_config_get_int("CLOCK", "snooze_minutes", 10);
            a->snooze_until = (long long)time(NULL) + (minutes > 0 ? minutes : 10) * 60;
        } else {
            a->snooze_until = 0;
            if (!a->days) a->enabled = 0; /* a one-time alarm is done once it has rung */
        }
    } else {
        XisTimer *t = xis_clock_find_timer(&c, g_id);
        if (!t) return;
        if (answer == ANSWER_PLUS_MINUTE) {
            t->end_ms = xis_clock_now_ms() + 60 * 1000;
        } else {
            t->end_ms = 0;
            t->remaining_ms = t->duration_ms;
        }
    }
    xis_clock_save(&c);
}

static void on_answer(GtkWidget *btn, gpointer data)
{
    stop_sound();
    apply_answer((Answer)GPOINTER_TO_INT(data));
    gtk_widget_destroy(gtk_widget_get_toplevel(btn));
}

static gboolean on_ring_timeout(gpointer data)
{
    GtkWidget *window = data;
    stop_sound();
    apply_answer(ANSWER_STOP);
    gtk_widget_destroy(window);
    return FALSE;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)data;
    if (ev->keyval == GDK_Escape) {
        stop_sound();
        apply_answer(ANSWER_STOP);
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    stop_sound();
    gtk_main_quit();
}

static void format_duration(long long ms, char *out, size_t outsz)
{
    long long s = (ms + 999) / 1000;
    if (s >= 3600) snprintf(out, outsz, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, outsz, "%02lld:%02lld", s / 60, s % 60);
}

int ring_run(int argc, char **argv)
{
    int at = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ring")) at = i;
    }
    if (!at || at + 2 >= argc || (strcmp(argv[at + 1], "alarm") && strcmp(argv[at + 1], "timer"))) {
        fprintf(stderr, "usage: %s --ring alarm|timer <id>\n", argv[0]);
        return 2;
    }
    g_is_alarm = !strcmp(argv[at + 1], "alarm");
    g_id = atoi(argv[at + 2]);

    /* main() ignores SIGCHLD for the launcher's sake; the player's exit
     * has to be seen here to loop it. */
    signal(SIGCHLD, SIG_DFL);
    xisserve_config_load();

    XisClock c;
    xis_clock_load(&c);
    char title[64], big[32], detail[XIS_CLOCK_LABEL_LEN + 64];
    if (g_is_alarm) {
        XisAlarm *a = xis_clock_find_alarm(&c, g_id);
        if (!a) return 1; /* deleted in the meantime */
        snprintf(title, sizeof(title), "%s", _("Alarme"));
        snprintf(big, sizeof(big), "%02d:%02d", a->hour, a->minute);
        snprintf(detail, sizeof(detail), "%s", a->label);
    } else {
        XisTimer *t = xis_clock_find_timer(&c, g_id);
        if (!t) return 1;
        snprintf(title, sizeof(title), "%s", _("Temporizador"));
        snprintf(big, sizeof(big), "%s", _("Tempo esgotado"));
        char dur[32];
        format_duration(t->duration_ms, dur, sizeof(dur));
        if (t->label[0]) snprintf(detail, sizeof(detail), "%s \xc2\xb7 %s", t->label, dur);
        else snprintf(detail, sizeof(detail), "%s", dur);
    }

    for (size_t i = 0; i < G_N_ELEMENTS(kSoundFiles) && !g_sound; i++) {
        if (g_file_test(kSoundFiles[i], G_FILE_TEST_EXISTS)) g_sound = kSoundFiles[i];
    }
    for (size_t i = 0; i < G_N_ELEMENTS(kPlayers) && !g_player; i++) {
        char *found = g_find_program_in_path(kPlayers[i]);
        if (found) {
            g_free(found);
            g_player = g_strdup(kPlayers[i]);
        }
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_urgency_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(window), 16);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *title_label = gtk_label_new(title);
    gtk_box_pack_start(GTK_BOX(vbox), title_label, FALSE, FALSE, 0);

    char *big_esc = g_markup_escape_text(big, -1);
    char *markup = g_strdup_printf("<span size=\"xx-large\" weight=\"bold\">%s</span>", big_esc);
    GtkWidget *big_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(big_label), markup);
    g_free(markup);
    g_free(big_esc);
    gtk_box_pack_start(GTK_BOX(vbox), big_label, FALSE, FALSE, 0);

    if (detail[0]) {
        GtkWidget *detail_label = gtk_label_new(detail);
        gtk_label_set_line_wrap(GTK_LABEL(detail_label), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), detail_label, FALSE, FALSE, 0);
    }

    GtkWidget *buttons = gtk_hbox_new(TRUE, 8);
    GtkWidget *stop_btn = gtk_button_new_with_label(_("Parar"));
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_answer), GINT_TO_POINTER(ANSWER_STOP));
    GtkWidget *other_btn;
    if (g_is_alarm) {
        int minutes = xisserve_config_get_int("CLOCK", "snooze_minutes", 10);
        char snooze[64];
        snprintf(snooze, sizeof(snooze), _("Soneca (%d min)"), minutes > 0 ? minutes : 10);
        other_btn = gtk_button_new_with_label(snooze);
        g_signal_connect(other_btn, "clicked", G_CALLBACK(on_answer), GINT_TO_POINTER(ANSWER_SNOOZE));
    } else {
        other_btn = gtk_button_new_with_label(_("+1 min"));
        g_signal_connect(other_btn, "clicked", G_CALLBACK(on_answer), GINT_TO_POINTER(ANSWER_PLUS_MINUTE));
    }
    gtk_box_pack_start(GTK_BOX(buttons), other_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), stop_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), buttons, FALSE, FALSE, 0);

    gtk_widget_set_size_request(window, 280, -1);
    gtk_widget_show_all(vbox);

    /* Centered on the monitor in use, same lookup as --session. */
    int ox = 0, oy = 0, ow = 0, oh = 0;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!xisserve_resolve_active_output(dpy, GDK_ROOT_WINDOW(), &ox, &oy, &ow, &oh)) {
        GdkScreen *screen = gdk_screen_get_default();
        ow = gdk_screen_get_width(screen);
        oh = gdk_screen_get_height(screen);
    }
    GtkRequisition req;
    gtk_widget_size_request(window, &req);
    gtk_window_move(GTK_WINDOW(window), ox + (ow - req.width) / 2, oy + (oh - req.height) / 2);

    gtk_widget_show(window);
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(stop_btn);

    play_once();
    int ring_minutes = xisserve_config_get_int("CLOCK", "ring_minutes", 5);
    g_timeout_add_seconds((guint)(ring_minutes > 0 ? ring_minutes : 5) * 60, on_ring_timeout, window);

    gtk_main();
    g_free(g_player);
    return 0;
}
