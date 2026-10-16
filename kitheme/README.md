# kitheme

One widget theme for every toolkit on the desktop: GTK2, GTK3, GTK4, Qt5 and
Qt6 draw the same buttons, fields, progress bars and menus from a single
configuration. It is the KiDesktop counterpart of QtCurve, written in C, and
configured from `kiconf`'s **KiTheme** tab.

## How it works

```
kitheme.conf (preset + overrides)
        |
  core/libkitheme.a   C + Cairo: config, colors, one renderer per element
        |
  +-----------+------------------+----------------------+
  gtk2/        export/              qt/
  GTK2 engine  kitheme-export       QStyle plugin (the only C++ file)
               -> GTK2 gtkrc
               -> GTK3/GTK4 CSS + PNG assets
```

- **GTK2**: `libkitheme.so` is a real engine. Every `draw_*` call goes to
  libkitheme, and the colors come from `kitheme.conf`. Colors that an
  application or rc file set explicitly are left alone.
- **GTK3/GTK4** have no engines, so `kitheme-export` writes CSS on top of
  the toolkit's built-in theme. Check boxes and radio buttons are PNGs drawn
  by the same renderer. All of this is static, so there is no cost at run
  time.
- **Qt5/Qt6**: `kitheme.so` (in `plugins/styles`) is a `QProxyStyle` over
  Fusion. Fusion keeps the layout of anything not overridden. Elements are
  drawn by libkitheme straight into `QImage` memory (the Cairo and Qt pixel
  formats are identical) and cached in `QPixmapCache`. The plugin shows up
  as **KiTheme** wherever Qt styles are listed, KDE's System Settings
  included.

libkitheme is a static library compiled into each adapter, so there is no
shared library or ABI to maintain.

## Configuration

`$XDG_CONFIG_HOME/kitheme/kitheme.conf` (default `~/.config/kitheme/`) uses
`key = value` lines. `preset = NAME` loads `presets/NAME.conf` (the shape:
style, radius, sizes) first, `colors = NAME` then loads the color scheme
`colors/NAME.conf`, and the keys that follow override both. With no file,
the `oxygen` preset and the `claro` scheme are used. Presets carry no
colors, so any preset combines with any scheme.

| key | values |
|---|---|
| `style` | `soft` (gradients and glow, Oxygen-like), `classic` (two-pixel bevels, Windows 9x), `flat` |
| `window_bg` `window_fg` `base_bg` `base_fg` `button_bg` `button_fg` `selection_bg` `selection_fg` `accent` `progress` `menu_bg` `menu_fg` `tooltip_bg` `tooltip_fg` | `#rrggbb` |
| `radius` | corner radius in px (`classic` ignores it) |
| `relief` `gradient` `shadow` | `0`..`1` |
| `focus` | `glow`, `dotted`, `none` |
| `hover_glow` | `0`/`1`: accent outline under the mouse |
| `progress_thickness` | px |
| `progress_text` | `auto` (when the application asks for it) or `always` (the percentage inside the bar) |
| `progress_stripes` | `0`/`1`: diagonal stripes; in `classic`, Windows-style blocks |
| `progress_animation` | `0`/`1`: moving stripes (one redraw every 50 ms while the bar is visible) |
| `scrollbar_width` `check_size` | px |
| `scrollbar_arrows` | `0`/`1` |

Presets are looked up in `$KITHEME_PRESETS`, then
`~/.local/share/kitheme/presets`, then `<prefix>/share/kitheme/presets`;
color schemes the same way in `$KITHEME_COLORS` and the `colors`
directories. A new preset or scheme is just a new file, with no rebuild
needed. Installed schemes: `claro`, `escuro`, `classico`. kiconf's KiTheme
tab lists them, edits a copy and saves your own with *Salvar como...*; the
**Aparência** tab offers the same list to recolor third-party GTK/Qt
themes, with *Sem cores personalizadas* (the default) leaving every theme,
KiTheme included, with its own colors.

## Choosing and applying it

- In `kiconf`, the **Aparência** tab sets KiTheme as the GTK2/GTK3/GTK4
  theme and the Qt style. Its *Usar KiTheme em tudo* button selects all of
  them at once.
- When KiTheme is installed, `kiconfd` defaults to it, and `kisession`
  exports `qt_style` as `QT_STYLE_OVERRIDE` for the session.
- **Aplicar** in the KiTheme tab saves `kitheme.conf`, then runs
  `kitheme-export --notify`, which regenerates `~/.themes/KiTheme` and
  makes running GTK2 apps re-read their rc files. It also sends SIGHUP to
  `kiconfd`. GTK3/4 only reload when the theme *name* changes, so `kiconfd`
  publishes the updated theme as `~/.themes/.KiTheme-reload` (a symlink to
  KiTheme). Qt apps watch `kitheme.conf` themselves. Everything already
  running updates, with no restart.
- Outside KiDesktop, KiTheme is an ordinary theme: pick it in KDE's System
  Settings, lxappearance or qt5ct like any other.

`kitheme-export --sample out.png` renders the preview picture from a
configuration, without any toolkit.

## Cost

Measured on the same test window, PSS and CPU over 5 s with an animated
busy bar:

| | PSS | CPU |
|---|---|---|
| Qt6 + KiTheme | 24.2 MB | 4 ticks |
| Qt6 + Fusion | 23.9 MB | 17 ticks |
| GTK2 + KiTheme | 5.19 MB | 0 |
| GTK2, no theme | 5.07 MB | 0 |

No GPU or VRAM is involved: drawing is Cairo on the CPU, as the toolkits
already do.

## Limits

- **GTK3** places the progress text over the bar by CSS, in one color with
  a halo. Only GTK2 and Qt split it at the bar's edge.
- **GTK4** draws that text before the bar, so it stays above the bar there.
- Whether a progress bar shows text at all is the application's decision in
  GTK3/4. `progress_text = always` only reaches GTK2 and Qt.
- **libadwaita** applications ignore GTK themes.
- **Qt4** is not supported.

## Packaging

The Qt plugins link Qt, but only Qt applications load them, so
`debian/rules` leaves them out of `kidesktop-core`'s dependency scan: Qt is
never a dependency of KiDesktop. On a machine without Qt they are a few
hundred kB of unused files. The packages that build them are build
dependencies only.

## Building

`make` builds the static library, the GTK2 engine, `kitheme-export`,
`build/KiTheme` (the copy installed in `/usr/share/themes`), and a Qt
plugin for each Qt version whose development files are installed (Qt5
through pkg-config, Qt6 through `qmake6`). It needs `cairo`, `gtk+-2.0`
and `x11`.

## Testing without touching the session

All of this runs in a nested Xephyr with a throwaway `HOME`:

```sh
export HOME=/tmp/kt-home XDG_CONFIG_HOME=/tmp/kt-home/.config DISPLAY=:37
export KITHEME_PRESETS=$PWD/presets KITHEME_COLORS=$PWD/colors GTK_PATH=/tmp/kt-gtkpath   # 2.10.0/engines/libkitheme.so -> gtk2/libkitheme.so
unset XDG_CURRENT_DESKTOP KDE_FULL_SESSION                       # keep Plasma's platform theme out
./export/kitheme-export                                          # -> $HOME/.themes/KiTheme
QT_PLUGIN_PATH=/tmp/kt-qtplug QT_STYLE_OVERRIDE=KiTheme some-qt-app   # styles/kitheme.so -> qt/qt5/kitheme.so
```
