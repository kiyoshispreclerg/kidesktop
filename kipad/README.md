# kipad

Simple tabbed text editor for KiDesktop (GTK2).

- Tabs: several files at once, drag to reorder, `Ctrl+PgUp/PgDn` to switch,
  `Ctrl+W` closes (asks to save changes)
- Font and size from *View > Font...*; the size also changes with
  `Ctrl`+mouse wheel, `Ctrl+Plus` / `Ctrl+Minus`, and `Ctrl+0` resets it
- *View > Word wrap* wraps long lines to the window width
- *View > Line numbers* shows or hides the numbers beside the text
- `Ctrl+F` opens a search bar at the bottom: `Enter` goes to the next
  occurrence, `Ctrl+Enter` to the previous one, wrapping around the file;
  `Esc` closes it. The `+` button opens a dialog with *Match case*, *Search in
  all open files* and the list of occurrences per open file (click one to
  jump to it, double-click to jump and close the dialog)
- Single window: with *View > Open files in the same window* on (the
  default), `kipad FILE` while kipad is running opens FILE as a tab of that
  window (a plain `kipad` just raises it); `kipad --new-window FILE` (`-n`)
  always opens a window of its own. A socket in `$XDG_RUNTIME_DIR` does the
  hand-over, one per display
- Right-click on a tab: *Close*, *Reopen* (reload from disk), *Detach* (reopen
  the file in another window and close the tab) and *Copy path*; Reopen and
  Detach warn that unsaved changes are lost
- The status bar shows the cursor's line and column
- Open, save, save as, undo/redo (`Ctrl+Z`, `Ctrl+Shift+Z` or `Ctrl+Y`),
  cut/copy/paste, select all. Files are read as UTF-8 (ISO-8859-1 when they
  are not) and always saved as UTF-8

Cursor positions (last 200 files) and the open tabs are remembered in
`~/.local/state/kipad/state`: files reopen at the last cursor position, and
`kipad` with no arguments reopens the tabs the last window had (closing the
last tab on purpose starts empty next time). With several windows, the last
one closed decides the tab list.

Settings (font, line numbers, word wrap, single window) are in `~/.config/kipad.conf`, written
whenever they change. Command line: `kipad [FILE...]`.

Not yet: replace, syntax highlighting.
