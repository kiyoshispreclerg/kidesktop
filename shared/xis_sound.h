/* xis_sound - the desktop-wide sound-theme player: any KiDesktop program
 * (or, through the xis-sound command, any app or script) calls
 * xis_sound_play("window-minimize") and the active theme decides what,
 * if anything, is heard.
 *
 * Settings are shared by every program, in one file --
 * $XDG_CONFIG_HOME/kidesktop-sound.conf (default ~/.config/...), written
 * by kiconf:
 *
 *     theme=Oxygen       # empty = silent
 *     enabled=1
 *     volume=80          # 0-100
 *     sound.window-close=/path/x.ogg   # per-event file; "none" = silent
 *
 * It is re-checked (a stat) at most every 2 seconds, so a change applies
 * to running programs with no reload plumbing, and the first play loads
 * it on its own: a caller needs no init call.
 *
 * Event names are open-ended: a program is free to invent one and a theme
 * to define it. The freedesktop sound-theme names ("bell",
 * "dialog-error", "message-new-instant", "trash-empty", "desktop-login",
 * ...) are used as-is; KiDesktop-specific ones are namespaced by what
 * emits them: window-minimize, window-close, window-maximize,
 * window-shade, window-unshade, window-all-desktops,
 * window-all-desktops-not, window-move, window-move-stop, login, logout.
 * kifiles emits folder-open, folder-back, folder-forward, folder-up,
 * file-trash, trash-empty, complete (a long copy/move finished),
 * dialog-error and device-removed (safe to unplug); kiterm emits bell and
 * complete (a program finished in a tab or window nobody was watching).
 *
 * Where an event's file comes from, first match wins:
 *   1. the KiDesktop theme <name>/sounds.conf under
 *      $XDG_DATA_HOME/kidesktop/sounds/, /usr/local/share/kidesktop/sounds/
 *      or /usr/share/kidesktop/sounds/, one "event=file" per line (file
 *      relative to that directory, or absolute);
 *   2. a freedesktop sound theme of the same name (index.theme, Inherits=,
 *      <dir>/<event>.oga|ogg|wav under .../share/sounds/), ending at the
 *      "freedesktop" theme;
 *   3. the KDE-style /usr/share/sounds/<Theme>-<Event>.ogg files
 *      ("Oxygen", "KDE"), for the window events above.
 *
 * Fire-and-forget: the file goes to pw-play (or paplay) in a double-forked
 * child, no shell, so the caller's loop never waits on audio. No player,
 * no file, or sounds off: a silent no-op. The same event repeating within
 * 100 ms plays once. */
#ifndef XIS_SOUND_H
#define XIS_SOUND_H

#include <stdbool.h>
#include <stddef.h>

/* Plays `event` from the active theme; no-op if there is nothing to play. */
void xis_sound_play(const char *event);

/* Forces the next play to re-read the settings and theme now, instead of
 * waiting out the 2 s check. */
void xis_sound_reload(void);

/* For a settings UI: the file `theme` would give `event` (ignoring the
 * per-event overrides), and an unconditional play of a file at a volume,
 * bypassing the enabled/theme settings, to preview before saving. */
bool xis_sound_theme_file(const char *theme, const char *event, char *out, size_t sz);
void xis_sound_play_file(const char *path, int volume_pct);

#endif
