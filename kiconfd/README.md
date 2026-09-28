# kiconfd

Settings daemon for KiDesktop. Reads a central config file and applies it
to every toolkit the desktop cares about, staying resident so a reload can
reapply everything without restarting anything. This is the daemon half of
`kiconf` (the GTK2 settings app) -- `kiconf` writes config files and signals
`kiconfd`, no separate control protocol.

It's also the one thing in the session that runs the night-light schedule
(set from `kiconf`'s Energia tab) on a timer, popping an OSD toast on
`xispanel` whenever it actually flips the tint on or off.

## What it applies

- **XSETTINGS** -- theme/icon theme/font/cursor, live, reaching every
  already-running app. Always applied.
- **Xresources** (`RESOURCE_MANAGER`) -- `Xcursor.theme`/`size`, `Xft.font`.
  Always applied.
- **GTK2** (`~/.gtkrc-2.0`), **GTK3/4** (`~/.config/gtk-{3,4}.0/settings.ini`
  + a generated color-scheme CSS) -- only written when
  `export_to_other_desktops` is on.
- **Qt5/6** (`qt{5,6}ct.conf` + a generated QPalette color scheme) -- only
  read when `qt{5,6}ct` is installed and selected as the platform theme;
  only written when `export_to_other_desktops` is on.
- **Screens** -- replays the layout `kiconf`'s Telas tab saved via one
  `xrandr` call, once at startup only.
- **Input** -- NumLock-on-start and the two XiS keyboard flags
  (ToggleModifiersOnPress/KickHotkeysOnRelease), applied at startup and on
  reload.
- **wx apps** -- nothing to do; wxGTK already follows the GTK settings.

## Config

`$XDG_CONFIG_HOME/kiconfd.conf` (fallback `~/.config/kiconfd.conf`), simple
`key = value` lines, `#` comments:

```
cursor_theme       = <Xcursor theme name>
cursor_size        = <pixel size>
color_bg           = #rrggbb
color_fg           = #rrggbb
color_base         = #rrggbb
color_accent       = #rrggbb
color_selection_bg = #rrggbb
color_selection_fg = #rrggbb
font_general       = <Pango font description, e.g. "Sans 10">
font_monospace     = <Pango font description, e.g. "Monospace 10">
gtk2_theme         = <GTK2 theme name>
gtk3_theme         = <GTK3 theme name>
gtk4_theme         = <GTK4 theme name>
icon_theme         = <icon theme name>
qt_style           = <QStyle name, e.g. "Fusion">
export_to_other_desktops = 0|1   # off by default, see below
```

Any key missing the first time is filled with a default and saved back.

Screens and input settings live in their own separate files
(`kiconfd-screens.conf`, `kiconfd-input.conf`) written by `kiconf`, since
those tabs rewrite their own file rather than the whole config.

`export_to_other_desktops` (off by default) controls whether kiconfd also
writes the shared, not-KiDesktop-specific files other desktops' GTK/Qt apps
read (`~/.gtkrc-2.0`, `gtk-{3,4}.0/settings.ini`, `qt{5,6}ct.conf`, ...).
With it off, theming still works fully within a KiDesktop session through
XSETTINGS and Xresources; the one thing that needs those shared files
either way is the custom color palette, which has no session-only channel.
Turning it on does not undo files a prior export already wrote.

## Signals

`SIGHUP` reloads the config and reapplies everything (except the
once-at-startup screens layout, which Telas already applies live itself).

## Logging

Off by default. Pass `--log`, or set `KICONFD_LOG=1` in the environment,
to redirect stdout/stderr to `$XDG_CONFIG_HOME/kiconfd.log` instead.
