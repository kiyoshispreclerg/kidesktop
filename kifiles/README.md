# kifiles

File manager for KiDesktop: GTK2 + GIO (no other dependencies; GVfs and udisks2
are used when installed).

- Icon and list views, zoom slider (16-256 px) or Ctrl+mouse wheel, sort by name / size / modified /
  type / extension (ascending or descending, folders first optional); in the
  list the column headers sort too
- Breadcrumb location bar (click the empty part, or `Ctrl+L`, to type a path;
  folders complete as you type), back / forward / up, mouse buttons 8 and 9
- Tabs (`Ctrl+T` new, `Ctrl+W` close, `Ctrl+Tab` / `Ctrl+PgDn` and
  `Ctrl+Shift+Tab` / `Ctrl+PgUp` to switch), between the menu bar and the
  toolbar. Drag to reorder; middle click closes. The tab menu has new tab,
  detach (closes it and opens its folder in a new window), close the others
  and close. Middle click on a folder, or "Open in New Tab", opens it in a
  background tab. Each tab has its own panes and split view; the toolbar and
  places sidebar are shared and follow the current tab. Closing the last tab
  closes the window
- Split view (`F3`): two folders side by side, drag between them
- Places sidebar (resizable, up to half the window): standard folders, bookmarks (shared with GTK3's
  `~/.config/gtk-3.0/bookmarks`) and devices with a thin usage bar under the name. Their context menu
  unmounts, or powers the drive off when it can ("Safely Remove") and says when
  it is safe to unplug. Unmounted volumes are listed; a click mounts them
- Copy / cut / paste (interoperable with GNOME/KDE/Thunar file managers),
  drag and drop (also to and from other applications, onto the sidebar and
  onto the Trash), move to trash, restore, delete, rename, new folder / file,
  **undo** (`Ctrl+Z`: move, copy, rename, trash, create)
- Name conflicts: replace / merge / keep both / skip, with "apply to all"
- Copy and move run in a worker thread, with pause and cancel. Progress goes
  to the desktop's job server (`org.kde.JobViewServerV2`: xispanel's toast,
  or Plasma's) like Dolphin's; kifiles' own dialog is the fallback when
  there's none. Closing the window lets running operations finish, and a
  copy that may not fit asks first
- Hover highlight: the item under the pointer gets a translucent box in the
  GTK theme's selection color, without selecting it (View menu toggle,
  `hover_highlight=` in the config). GTK2 views have no such thing, so it is
  drawn by `kf_hover.c`; the xisserve launcher's hover *selects* instead
- Sounds through the KiDesktop sound theme (`shared/xis_sound.h`, set up in
  kiconf's Sons tab): `folder-open`, `folder-back`, `folder-forward`,
  `folder-up`, `file-trash`, `trash-empty`, `complete` (a copy/move that was
  long enough to show a progress dialog), `dialog-error`, `device-removed`.
  Whether anything is heard depends on the theme; no theme, no sound
- Thumbnails (see below), quick filter (`Ctrl+F`), hidden files (`Ctrl+H`)
- Status bar: folder / file counts and total size (of the selection when there
  is one), free space of the current device with a bar

## Thumbnails

Freedesktop thumbnail cache (`~/.cache/thumbnails/{normal,large}`), shared with
other file managers. Images are made with gdk-pixbuf; every other format goes
through the external thumbnailers listed in `thumbnailers/*.thumbnailer` under
`$XDG_DATA_DIRS` and `~/.local/share` (the same files GNOME and Tumbler use:
ffmpegthumbnailer, evince, librsvg...). To thumbnail a new format, install a
`.thumbnailer` file for it. Only the rows near the visible area are requested,
and only local files are thumbnailed.

## Settings

`~/.config/kifiles.conf` (flat `key=value`, written on exit; `view` is the default view mode for every folder): `view`, `zoom`,
`sort`, `sort_desc`, `folders_first`, `show_hidden`, `thumbnails`, `split`,
`show_places`, `window_width`, `window_height`, `places_width`.

Started with no folder argument, kifiles reopens the tabs it had (and the two
folders of each split tab) and goes back to the tab that was showing. They are
the repeated `tab=URI` / `tab=URI URI` lines plus `current_tab`. The file is
also written on SIGTERM/SIGHUP and when the X connection drops, so logging out
keeps it. A window opened on a given folder leaves the saved tabs alone.

## Using it as the default

```sh
xdg-mime default kifiles.desktop inode/directory
```

"Open terminal here" (`F4`) starts `kiterm` when installed, else
`x-terminal-emulator`.

## Not in 0.1.0

An editable permissions dialog, folder sizes in Properties, a places
entry for network locations, `udisksctl` fallback when GVfs is missing,
X-DENSITY scaling, a kiconf tab.
