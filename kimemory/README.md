# kimemory

Clipboard history for KiDesktop. `kimemoryd` records every CLIPBOARD copy
together with the window, app and document it came from, into a plain-text
history. PRIMARY is ignored (off in the KiDesktop X server).

## How a copy is handled

On each new CLIPBOARD owner (XFixes), kimemoryd asks for `TARGETS` and
sorts the copy:

- **secret**: `x-kde-passwordManagerHint` is offered (KeePassXC and other
  password managers), or the source app is in `exclude`. Nothing is read.
- **rich**: app-private targets (LibreOffice/Qt/KDE/GIMP/Krita formats,
  images other than png) or the app is in `never_takeover`. Only the
  canonical representations are fetched: text, `text/html`,
  `text/uri-list`, file-manager copy lists, `image/png`.
- **simple**: everything else. Every target is mirrored, so the copy can
  be served exactly as the app offered it.

The source is the managed toplevel of the X client that owns the
selection, falling back to the active window. Copying the same content
again moves the entry to the top and adds the new source.

## Config

`$XDG_CONFIG_HOME/kimemory.conf`, `key=value`, `SIGHUP` reloads:

| key | default | |
|---|---|---|
| `max_entries` | 200 | favourites don't count |
| `persist` | `all` | `all`, `favorites` (the rest stays in RAM only), `none` |
| `takeover` | `simple` | `simple`, `onexit`, `never` |
| `exclude` | `keepassxc,KeePassXC` | WM_CLASS or exe basenames never recorded |
| `never_takeover` | office/graphics apps | always treated as rich |
| `max_text_kb` | 1024 | |
| `max_image_mb` | 20 | |
| `max_mirror_mb` | 4 | extra targets mirrored for a simple copy |

## Data

`$XDG_DATA_HOME/kimemory/` (mode 0700):

```
index.tsv       I id ts type bytes fav hash       (type 0 text, 1 link, 2 files, 3 image)
                R id n target type format len     -> items/<id>.<n>
                S id ts win app title doc          copied from
                D id ts win app title doc          pasted into
items/<id>.<n>  raw data of one representation
```

Tab-separated, `\t` `\n` `\\` escaped, oldest first; rewritten whole on
every change.

## Permissions (xisguard)

In a session guarded by xisguard, kimemoryd needs `MANAGE` (reading other
clients' window properties) and `SELECTION`, e.g. in `perms.conf`:

```
ALLOW MANAGE /usr/local/bin/kimemoryd
ALLOW SELECTION /usr/local/bin/kimemoryd
```
