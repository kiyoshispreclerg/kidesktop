# kipad

Simple tabbed text editor for KiDesktop (GTK2).

- Tabs: several files at once, drag to reorder, `Ctrl+PgUp/PgDn` to switch,
  `Ctrl+W` closes (asks to save changes)
- Font and size from *View > Font...*; the size also changes with
  `Ctrl`+mouse wheel, `Ctrl+Plus` / `Ctrl+Minus`, and `Ctrl+0` resets it
- *View > Word wrap* wraps long lines to the window width
- *View > Line numbers* shows or hides the numbers beside the text
- The status bar shows the cursor's line and column
- Open, save, save as, undo/redo (`Ctrl+Z`, `Ctrl+Shift+Z` or `Ctrl+Y`),
  cut/copy/paste, select all. Files are read as UTF-8 (ISO-8859-1 when they
  are not) and always saved as UTF-8

Cursor positions (last 200 files) and the open tabs are remembered in
`~/.local/state/kipad/state`: files reopen at the last cursor position, and
`kipad` with no arguments reopens the tabs the last window had (closing the
last tab on purpose starts empty next time). With several windows, the last
one closed decides the tab list.

Settings (font, line numbers, word wrap) are in `~/.config/kipad.conf`, written
whenever they change. Command line: `kipad [FILE...]`.

Not in 0.1.0: search/replace, syntax highlighting.
