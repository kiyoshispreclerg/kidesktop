# kipad

Simple tabbed text editor for KiDesktop (GTK2).

- Tabs: several files at once, drag to reorder, `Ctrl+PgUp/PgDn` to switch,
  `Ctrl+W` closes (asks to save changes)
- Font and size from *View > Font...*; the size also changes with
  `Ctrl`+mouse wheel, `Ctrl+Plus` / `Ctrl+Minus`, and `Ctrl+0` resets it
- *View > Line numbers* shows or hides the numbers beside the text
- The status bar shows the cursor's line and column
- Open, save, save as, undo/redo (`Ctrl+Z`, `Ctrl+Shift+Z` or `Ctrl+Y`),
  cut/copy/paste, select all. Files are read as UTF-8 (ISO-8859-1 when they
  are not) and always saved as UTF-8

Settings (font, line numbers) are in `~/.config/kipad.conf`, written
whenever they change. Command line: `kipad [FILE...]`.

Not in 0.1.0: search/replace, word wrap, syntax highlighting, session restore.
