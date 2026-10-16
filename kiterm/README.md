# kiterm

Tabbed terminal emulator for KiDesktop: GTK2 + VTE 0.28 (`libvte-dev`, the
GTK2 series -- present on Ubuntu 22.04/24.04, Debian 12/13 and Devuan 5/6).

- Tabs (double-click renames, right-click menu, middle-click closes, drag to reorder, tab bar position configurable)
- Find bar with case/regex options (`Ctrl+Shift+F`, Enter / Shift+Enter)
- Colors: presets or per-color, 16-color palette, cursor shape/blink
- Transparency (`opacity=`; needs a compositor, set before the window opens)
- Sounds through the KiDesktop sound theme (`shared/xis_sound.h`): the bell
  plays the `bell` event, and a program that finishes in another tab or an
  unfocused window plays `complete`, marks the tab with a dot and flags the
  window (only if it ran at least `notify_done_secs`, default 10 s; the
  background watch only runs while the window is unfocused or has several tabs)
- Context menu > Send signal: INT, TERM, KILL, HUP, STOP, CONT, USR1... to
  the running program (the terminal's foreground process group)
- Ctrl+click opens URLs, dropped files are pasted as quoted paths
- Zoom with `Ctrl+Plus` / `Ctrl+Minus` / `Ctrl+0` or `Ctrl`+mouse wheel; font
  family and size are in Preferences

Shortcuts: `Ctrl+Shift+T` new tab, `+N` new window, `+W` close tab, `+C`/`+V`
copy/paste, `+R` rename tab, `+,` preferences, `Ctrl+PgUp/PgDn` switch tab,
`Ctrl+Shift+PgUp/PgDn` move tab, `Alt+1..9` jump to tab, `F11` fullscreen.

Settings are in `~/.config/kiterm.conf` (flat `key=value`, written by the
preferences dialog). Command line: `kiterm [--working-directory=DIR]
[--title=T] [--geometry=COLSxROWS] [-e COMMAND ARGS...]`.

Not in 0.1.x: split panes, profiles, X-DENSITY scaling, kiconf tab.
