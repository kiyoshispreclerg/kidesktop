# kifiles

File manager for KiDesktop: GTK2 + GIO (no other dependencies; GVfs and udisks2
are used when installed).

- Icon and list views, zoom slider (16-256 px), sort by name / size / modified /
  type / extension (ascending or descending, folders first optional); in the
  list the column headers sort too
- Breadcrumb location bar (click the empty part, or `Ctrl+L`, to type a path;
  folders complete as you type), back / forward / up, mouse buttons 8 and 9
- Split view (`F3`): two folders side by side, drag between them
- Places sidebar: standard folders, bookmarks (shared with GTK3's
  `~/.config/gtk-3.0/bookmarks`) and devices with a usage bar. The eject button
  unmounts and powers the drive off when it can, and says when it is safe to
  unplug. Unmounted volumes are listed; a click mounts them
- Copy / cut / paste (interoperable with GNOME/KDE/Thunar file managers),
  drag and drop (also to and from other applications, onto the sidebar and
  onto the Trash), move to trash, restore, delete, rename, new folder / file,
  **undo** (`Ctrl+Z`: move, copy, rename, trash, create)
- Name conflicts: replace / merge / keep both / skip, with "apply to all"
- Copy and move run in a worker thread with a progress dialog and cancel
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

`~/.config/kifiles.conf` (flat `key=value`, written on exit): `view`, `zoom`,
`sort`, `sort_desc`, `folders_first`, `show_hidden`, `thumbnails`, `split`,
`show_places`, `window_width`, `window_height`, `places_width`.

## Using it as the default

```sh
xdg-mime default kifiles.desktop inode/directory
```

"Open terminal here" (`F4`) starts `kiterm` when installed, else
`x-terminal-emulator`.

## Not in 0.1.0

Tabs, an editable permissions dialog, folder sizes in Properties, a places
entry for network locations, `udisksctl` fallback when GVfs is missing,
X-DENSITY scaling, a kiconf tab.
