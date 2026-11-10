# kifiles service menus

A program adds entries to kifiles' right-click menu by installing a `.desktop`
file. The format is KDE's *service menu* format (the one Dolphin and Ark use),
so service menus written for KDE work here unchanged and the other way round.
There is nothing to compile and nothing is loaded into kifiles: an action is an
ordinary command line, and a broken one cannot take the file manager down.

Menus are about **selected items**. They are not shown on the empty background
of a folder, nor inside the trash.

## Where to put the file

Searched in this order; the first file with a given **name** wins, so a file in
your home overrides a system one.

| Directory (under `$XDG_DATA_HOME`, then each of `$XDG_DATA_DIRS`) | For |
|---|---|
| `kifiles/actions/` | KiDesktop programs (kizip, ...): the native place |
| `kio/servicemenus/` | KDE service menus (Plasma 5.x/6) |
| `kservices5/ServiceMenus/` | older KDE service menus |

Normally `~/.local/share/kifiles/actions/` for a user and
`/usr/share/kifiles/actions/` for a package. The directories are rescanned at
every right-click, so a new or edited file shows up at once; there is nothing
to restart.

To hide a system file without deleting it, put a file with the same name in
your home containing `Hidden=true` under `[Desktop Entry]`.

## A complete example

`/usr/share/kifiles/actions/kizip-extract.desktop`:

```ini
[Desktop Entry]
Type=Service
MimeType=application/zip;application/x-tar;application/x-7z-compressed;
Actions=Here;Subfolder;To;
X-KDE-Submenu=Extract
X-KDE-Submenu[pt_BR]=Extrair
X-KDE-Priority=TopLevel
TryExec=kizip

[Desktop Action Here]
Name=Extract here
Name[pt_BR]=Extrair aqui
Icon=kizip
Exec=kizip --extract-here %F

[Desktop Action Subfolder]
Name=Extract to a subfolder
Name[pt_BR]=Extrair para subpasta
Icon=kizip
Exec=kizip --extract-subfolder %F

[Desktop Action To]
Name=Extract to...
Name[pt_BR]=Extrair para...
Icon=kizip
Exec=kizip --extract-to %F
```

More samples are in [examples/](examples/): copy one to
`~/.local/share/kifiles/actions/` and right-click something.

## `[Desktop Entry]` keys

| Key | Meaning |
|---|---|
| `Type` | `Service` (anything is accepted; not checked) |
| `MimeType` | **Required.** `;`-separated list of when the menu applies (below) |
| `Actions` | `;`-separated tokens, one `[Desktop Action <token>]` group each, shown in this order. Without it the entry itself (`Name`, `Exec`, `Icon`) is the single action |
| `X-KDE-Submenu` | Name of a submenu holding all the actions of the file. Files that name the same submenu share it. Can be localized (`X-KDE-Submenu[pt_BR]`) |
| `X-KDE-Priority` | `TopLevel`: shown in the main menu (the submenu itself, if there is one). Anything else, or absent: inside the generic **Actions** submenu |
| `X-KDE-RequiredNumberOfUrls` | Only for exactly this many selected items; a list (`1,2`) allows several counts |
| `X-KDE-MinNumberOfUrls`, `X-KDE-MaxNumberOfUrls` | Bounds on the number of selected items |
| `X-KDE-Protocols` | URI schemes it works on. Default `file` (local paths); `*` means any. Remote items are only offered if the scheme is listed |
| `TryExec` | Program that must exist (name in `PATH`, or absolute path); otherwise the file is skipped. Use it so uninstalled programs leave no dead entries |
| `Hidden` | `true` ignores the file (and, by name, any file in a later directory) |

Ignored, though common in KDE files: `ServiceTypes`, `X-KDE-ServiceTypes`,
`X-KDE-AuthorizeAction`, `OnlyShowIn`, `NotShowIn`.

### `MimeType`

Every selected item has to match at least one entry, otherwise the file is not
offered (one action cannot take a mix it doesn't understand).

| Entry | Matches |
|---|---|
| `all/all` | everything, files and folders |
| `all/allfiles` | any file, no folders |
| `inode/directory` | folders |
| `audio/*` | any subtype of `audio` |
| `image/png` | that type and its subtypes (`g_content_type_is_a`) |

Types are the shared-mime-info ones, e.g. `application/zip`,
`application/x-7z-compressed`, `application/vnd.rar`. `file --mime-type FILE`
shows one.

## `[Desktop Action <token>]` keys

| Key | Meaning |
|---|---|
| `Name` | **Required.** The label; can be localized (`Name[pt_BR]`) |
| `Exec` | **Required.** The command, with field codes (below) |
| `Icon` | Icon name or absolute path; optional |

### `Exec` field codes

The usual `.desktop` ones; the command is run without a shell, so use
`sh -c '...' sh %F` if you need one.

| Code | Replaced by |
|---|---|
| `%F` | all selected items as local paths, one argument each |
| `%f` | the first item as a path (the command runs once per item if several are selected and only `%f` is used) |
| `%U`, `%u` | the same as URIs |
| `%%` | a literal `%` |

An `Exec` with no file code gets no files. The command always runs with the
**folder being shown as working directory** and receives absolute paths, so
`--extract-here` can mean "next to the archive" by itself. Output is not shown:
a program that wants to report something must open its own window or send a
notification.

## Turning actions off

`~/.config/ki-fileactions.conf` (read by kifiles at every right-click, written
by the *Acoes de arquivos* tab of kiconf):

```
disabled=kizip-extract.desktop
disabled=kizip-compress.desktop:AddNamed
```

A bare file name turns off all of its actions; `<file>:<token>` only that one
(`-` is the token of a file without `Actions=`).

## Rules of thumb for authors

* Ship one file per feature and a clear `TryExec`.
* Keep `MimeType` as narrow as the action allows: *Compress* is `all/all`,
  *Extract* only the archive types, *Convert to...* `audio/*` or `image/*`.
* Prefer a CLI that opens its own progress window (or reports through the
  desktop's job-progress service) over `kiterm -e`; there is no output here.
* Name the submenu once the entries are three or more.
