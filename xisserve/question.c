/*
 * question.c - `xisserve --question --text=<pergunta> --button=<rotulo>:<valor>
 * [--button=<rotulo>:<valor> ...]`: a Zenity-style question popup with a
 * caller-defined set of answer buttons, each returning its own int value.
 *
 * Like `--menu` (see appmenu.c), this is **not** part of the xispanel
 * contract and **not** subject to the singleton/control-socket behavior
 * PROTOCOL.md describes for the launcher: it never takes the flock, never
 * opens or connects to xisserve.sock, and so can run any number of
 * instances at once -- a script or daemon asking two different questions
 * back to back gets two independent popups rather than the second one
 * being relayed into (and repositioning/retheming) the first's window.
 * main() dispatches here, before the singleton lock, the same way it
 * dispatches to appmenu_run() for --menu.
 *
 * The dialog itself is deliberately plain GTK2 -- a label and a row of
 * stock buttons, no --bg/--fg/--font theming like the launcher's own
 * window takes. It's a modal question, not a themed popup living
 * alongside xispanel; matching whatever GTK2 theme is already active on
 * the session is enough.
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "xisserve.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUESTION_MAX_BUTTONS 8

typedef struct {
    char label[128];
    int value;
} QuestionButton;

/* Per-button click context: which value this button answers with, and
 * where to write it -- out_value/answered are shared across every button
 * of one dialog, set by whichever one gets clicked. */
typedef struct {
    int value;
    int *out_value;
    gboolean *answered;
} ButtonCtx;

static void on_button_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    ButtonCtx *ctx = data;
    *ctx->out_value = ctx->value;
    *ctx->answered = TRUE;
    gtk_main_quit();
}

/* Covers every way the window can go away without a button click: the
 * WM's close button (destroy) and Escape (explicitly wired below, since a
 * plain GtkWindow -- unlike GtkDialog -- doesn't close on Escape by
 * itself). *answered stays FALSE either way, which question_run() below
 * reports as "no answer" rather than guessing a value for it. */
static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_main_quit();
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)data;
    if (ev->keyval == GDK_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

enum { QOPT_QUESTION = 1, QOPT_TEXT, QOPT_BUTTON };

static const struct option kQuestionOpts[] = {
    {"question", no_argument, 0, QOPT_QUESTION},
    {"text", required_argument, 0, QOPT_TEXT},
    {"button", required_argument, 0, QOPT_BUTTON},
    {0, 0, 0, 0},
};

static void question_usage(void)
{
    fprintf(stderr,
            "usage: xisserve --question --text=<pergunta> "
            "--button=<rotulo>:<valor> [--button=<rotulo>:<valor> ...] "
            "(1-%d buttons)\n",
            QUESTION_MAX_BUTTONS);
}

/* "<rotulo>:<valor>" -> buttons[n_buttons]. Splits on the *last* ':' so a
 * label is free to contain its own colons ("Ex: sim"); only the trailing
 * "...:<valor>" past it is required to be one. */
static int parse_button_arg(const char *arg, QuestionButton *out)
{
    const char *sep = strrchr(arg, ':');
    if (!sep || sep == arg) return 0;
    size_t label_len = (size_t)(sep - arg);
    if (label_len >= sizeof(out->label)) label_len = sizeof(out->label) - 1;
    memcpy(out->label, arg, label_len);
    out->label[label_len] = 0;
    out->value = atoi(sep + 1);
    return 1;
}

int question_run(int argc, char **argv)
{
    char text[1024] = "";
    QuestionButton buttons[QUESTION_MAX_BUTTONS];
    int n_buttons = 0;

    optind = 1;
    opterr = 0;
    int c;
    while ((c = getopt_long(argc, argv, "", kQuestionOpts, NULL)) != -1) {
        switch (c) {
        case QOPT_QUESTION: break; /* how main() knew to call us; nothing to do */
        case QOPT_TEXT: snprintf(text, sizeof(text), "%s", optarg); break;
        case QOPT_BUTTON:
            if (n_buttons >= QUESTION_MAX_BUTTONS) {
                fprintf(stderr, "xisserve --question: too many --button, ignoring \"%s\"\n", optarg);
                break;
            }
            if (!parse_button_arg(optarg, &buttons[n_buttons])) {
                fprintf(stderr, "xisserve --question: --button must be \"<rotulo>:<valor>\", got \"%s\"\n", optarg);
                break;
            }
            n_buttons++;
            break;
        default: break; /* unknown flag -- ignored, same contract as the launcher's own argv */
        }
    }

    if (!text[0] || n_buttons == 0) {
        question_usage();
        return 2;
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "xisserve");
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 12);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

    GtkWidget *bbox = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(bbox), 6);
    gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);

    int out_value = 0;
    gboolean answered = FALSE;
    ButtonCtx ctxs[QUESTION_MAX_BUTTONS];
    for (int i = 0; i < n_buttons; i++) {
        GtkWidget *btn = gtk_button_new_with_label(buttons[i].label);
        ctxs[i].value = buttons[i].value;
        ctxs[i].out_value = &out_value;
        ctxs[i].answered = &answered;
        g_signal_connect(btn, "clicked", G_CALLBACK(on_button_clicked), &ctxs[i]);
        gtk_container_add(GTK_CONTAINER(bbox), btn);
    }
    /* Last button (conventionally the "default"/affirmative one, same
     * left-to-right-ends-in-OK convention as the rest of this codebase's
     * dialogs) gets the focus and the Enter-key default, so a keyboard
     * user isn't required to reach for the mouse. */
    if (n_buttons > 0) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(bbox));
        GtkWidget *last = GTK_WIDGET(g_list_last(kids)->data);
        gtk_widget_set_can_default(last, TRUE);
        gtk_widget_grab_default(last);
        gtk_widget_grab_focus(last);
        g_list_free(kids);
    }

    gtk_widget_show_all(window);
    gtk_main();

    if (!answered) return 1; /* dismissed with no answer -- no stdout, exit 1 */

    printf("%d\n", out_value);
    fflush(stdout);
    /* Process exit codes are one byte; a caller relying on the exit
     * status rather than stdout only works for 0-255 answers, so that's
     * what's documented in PROTOCOL.md -- values outside it still print
     * correctly on stdout regardless. */
    return out_value & 0xff;
}
