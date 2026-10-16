/*
 * mpris.h - minimal MPRIS2 client for the --audio page: finds running
 * media players on the session bus so a stream's row can offer
 * previous/play-pause/next next to its volume. GDBus (already linked
 * through GTK's GIO), no extra dependency; every call has a short
 * timeout and a missing bus or player just yields an empty list.
 */
#ifndef XISSERVE_MPRIS_H
#define XISSERVE_MPRIS_H

#include <glib.h>

typedef struct {
    char bus[128];      /* org.mpris.MediaPlayer2.<name>[.instanceN] */
    char identity[128]; /* MediaPlayer2.Identity */
    char desktop[128];  /* MediaPlayer2.DesktopEntry, "" if absent */
    guint pid;          /* owner of the bus name, 0 if unknown */
    gboolean playing;
    gboolean can_next, can_prev;
} MprisPlayer;

/* Snapshot of every player on the bus: GPtrArray of owned MprisPlayer*,
 * never NULL. */
GPtrArray *mpris_list(void);

/* The player belonging to a pulse stream: same process id first, else a
 * case-insensitive name match between any of app name / binary / icon
 * name and the player's identity / desktop entry / bus name. NULL if none. */
const MprisPlayer *mpris_match(GPtrArray *players, guint pid, const char *app, const char *binary, const char *icon);

/* "PlayPause", "Next" or "Previous"; fire and forget. */
void mpris_command(const char *bus, const char *method);

#endif
