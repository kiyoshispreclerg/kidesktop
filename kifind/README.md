# kifind

File search for KiDesktop (GTK2), in the spirit of KDE's KFind. kifiles opens
it with `Ctrl+F` in the folder it is showing.

    kifind [FOLDER]

FOLDER is a path or a `file://` URI; without it the search starts in the
current directory.

## Searching

- **Name**: part of the name ("relat" finds "Relatório.odt"), or a pattern
  where `*` stands for any run of characters (`*.txt`, `rel*2024*`); a
  pattern with `*` must match the whole name. Only the name is matched, not
  the path. Empty finds everything (narrowed by the other options)
- **Include subfolders**, **Match case** (off: case-insensitive),
  **Include hidden files and folders** (off: dot-files are skipped and hidden
  folders are not entered)
- **Advanced**, every field optional and all of them combined:
  - Type: any of folders, images, audio, video, text, documents, archives,
    executables, other files. Judged by the name (no file is opened), like a
    file manager's icons; a file may be in more than one (a script is text
    and executable)
  - Created and Modified: from / to dates (both inclusive; type a date in
    your locale's format or `YYYY-MM-DD`, or pick it from the calendar).
    File systems that don't record creation times never match a creation
    date
  - Size: from / to, in bytes, KiB, MiB or GiB. With a size set, folders don't
    match
  - Owner and Group: any of the chosen ones. People (ids 1000 and up) and
    root are listed first, system accounts in a submenu

The walk runs in a thread and fills the list as it goes; **Stop** (or `Esc`)
ends it. Symlinked folders are not followed, and `/proc`, `/sys`, `/dev` and
`/run/user` are skipped unless searched directly. Names are matched before
anything else, so only matching files cost a `stat()` (an inode read; a seek
on a cold disk).

Results: double-click opens with the default application; the right-click
menu also opens the containing folder (in kifiles) and copies the path.
Columns sort by clicking their headers.

## Planned

- Search inside files (text contents), with a size limit and binary files
  skipped
- Remember the last options; save and reuse searches
- Drag results to other applications; select several and act on them
- Type by content (magic bytes) as an option, not only by name
