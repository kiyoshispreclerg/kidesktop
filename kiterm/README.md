# kiterm

Tabbed terminal emulator for KiDesktop: GTK2 + VTE 0.28 (`libvte-dev`, the
GTK2 series -- present on Ubuntu 22.04/24.04, Debian 12/13 and Devuan 5/6).

- Tabs (middle-click closes, drag to reorder, tab bar position configurable)
- Find bar with case/regex options (`Ctrl+Shift+F`, Enter / Shift+Enter)
- Colors: presets or per-color, 16-color palette, cursor shape/blink
- Transparency (`opacity=`; needs a compositor, set before the window opens)
- Ctrl+click opens URLs, dropped files are pasted as quoted paths
- Zoom with `Ctrl+Plus` / `Ctrl+Minus` / `Ctrl+0`

Shortcuts: `Ctrl+Shift+T` new tab, `+N` new window, `+W` close tab, `+C`/`+V`
copy/paste, `+R` rename tab, `+,` preferences, `Ctrl+PgUp/PgDn` switch tab,
`Ctrl+Shift+PgUp/PgDn` move tab, `Alt+1..9` jump to tab, `F11` fullscreen.

Settings are in `~/.config/kiterm.conf` (flat `key=value`, written by the
preferences dialog). Command line: `kiterm [--working-directory=DIR]
[--title=T] [--geometry=COLSxROWS] [-e COMMAND ARGS...]`.

Not in 0.1.0: split panes, profiles, X-DENSITY scaling, kiconf tab.
