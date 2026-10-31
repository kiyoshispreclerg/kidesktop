/* xis_keycapture.h - asking the user for a hotkey from a GTK program, by
 * running kiconf's xiskeycapture dialog (see the top comment of
 * kiconf/xiskeycapture.c for why it is a separate process).
 *
 * Asynchronous: xis_keycapture_start() returns at once and `done` runs
 * from the main loop when the dialog closes. Only one capture runs at a
 * time. Callers must keep a way to type the spec as text: the user can
 * ask for it (XIS_KEYCAPTURE_EDIT_TEXT), and it's the only way to bind a
 * bare Esc/Enter or a mouse button. */
#ifndef XIS_KEYCAPTURE_H
#define XIS_KEYCAPTURE_H

#include <gtk/gtk.h>

typedef enum {
    XIS_KEYCAPTURE_OK,        /* spec holds the new binding */
    XIS_KEYCAPTURE_CANCEL,    /* also: helper died or was killed */
    XIS_KEYCAPTURE_CLEAR,     /* user chose "no shortcut" */
    XIS_KEYCAPTURE_EDIT_TEXT, /* user wants the plain text field */
} XisKeyCaptureResult;

typedef struct {
    const char *title;            /* dialog title, usually the action's name; may be NULL */
    const char *current;          /* shown as "Atual: ..."; may be NULL */
    const char *modkey;           /* kiwm rows: mod_key= value ("meta"/"alt"), else NULL */
    gboolean allow_modifier_only; /* xispanel hotkey=: a lone modifier tap is valid */
} XisKeyCaptureOpts;

typedef void (*XisKeyCaptureDone)(XisKeyCaptureResult result, const char *spec, gpointer data);

/* FALSE if not started -- a capture is already running, or xiskeycapture
 * isn't installed; then `done` is never called and the caller should
 * fall back to text editing. `parent` (any widget) makes the dialog
 * transient for its window. */
gboolean xis_keycapture_start(GtkWidget *parent, const XisKeyCaptureOpts *opts,
                              XisKeyCaptureDone done, gpointer data);

gboolean xis_keycapture_running(void);

#endif /* XIS_KEYCAPTURE_H */
