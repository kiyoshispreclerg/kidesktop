# kiwm

**kiwm** is the stacking window manager for **KiDesktop** -- plain XCB (no Xlib), Cairo + Imlib2
for decoration, RandR for multi-monitor. KiDesktop is built with XiS/XLibre in mind but isn't
exclusive to it -- it targets X11 generally, and kiwm works the same standalone on any X server
(no compositor required); an optional compositor (`kicomp`) is planned alongside it but never a
dependency. See [kiwm-kicomp-projeto.md](kiwm-kicomp-projeto.md) for the full architecture/phased
plan this was built from, and [PROTOCOL.md](PROTOCOL.md) for the one custom (non-EWMH) protocol it
exposes.

### Status

**Early, but past first-prototype.** What works right now:

- map/unmap/move/resize/maximize/minimize/close, click-to-focus (or optional
  focus-follows-mouse), Alt-Tab-style window cycling.
- Virtual desktops tracked **independently per output** (not one global workspace number) --
  see [PROTOCOL.md](PROTOCOL.md).
- Already-open windows are picked up at startup, not just windows mapped afterward.
- `--replace`: proper ICCCM manager-selection handoff, and kiwm itself can later be `--replace`d
  cleanly by something else.
- Graceful shutdown on SIGTERM/SIGINT: every managed window is reparented back to root before
  kiwm exits, so windows survive a kill instead of vanishing with it.
- Dock/panel awareness: `_NET_WM_STRUT`/`_NET_WM_STRUT_PARTIAL`, correctly attributed per output
  (a panel on one monitor doesn't eat into a different monitor's usable area), feeding
  `_NET_WORKAREA` and maximize.
- Windows7/kwin-style edge-drag snapping (drag a titlebar to a screen edge to maximize/half-tile),
  corner-relative resize, double-click-titlebar-to-maximize, scroll-wheel shade.
- Window states beyond the basics: shade (`_NET_WM_STATE_SHADED`), keep-above
  (`_NET_WM_STATE_ABOVE`), sticky/keep-on-all-desktops (`_NET_WM_STATE_STICKY`, meaning "visible
  regardless of this window's own output's current desktop" -- see PROTOCOL.md).
- A small theme system (background image, button sprite sheet, per-focus colors) with a
  configurable titlebar element order -- see "Theming" below.
- Enough EWMH/ICCCM for a taskbar (xispanel's tasklist widget) to list/activate/close/minimize/
  maximize windows: `_NET_CLIENT_LIST(_STACKING)`, `_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`,
  `_NET_WM_STATE`, ICCCM `WM_STATE`, `_NET_WM_DESKTOP`, `_NET_SUPPORTING_WM_CHECK`,
  `_NET_WORKAREA`, `_NET_FRAME_EXTENTS`, `_NET_WM_ICON`.

Not implemented yet: `kicomp` compositor (no client-side compositing at all), fullscreen state,
resizing by grabbing the window's own edge/corner with no modifier held (only mod+right-click
resize exists so far), a real multi-layer stacking model (keep-above is enforced by re-raising on
demand rather than a proper stacking tier), global menu.

### Dependencies

- libxcb, libxcb-randr
- Cairo with the `cairo-xcb` backend
- Imlib2

### Building and running

```sh
make
./kiwm            # refuses to start if another WM already owns the screen
./kiwm --replace  # takes over from whatever WM is currently running
```

kiwm needs no compositor, no session manager, and no other XiS daemon to function on its own --
but it's meant to sit alongside the rest of KiDesktop:

- **[xispanel](../xispanel)**: a taskbar/panel reading kiwm's standard EWMH client list plus the
  custom per-output desktop protocol (PROTOCOL.md) for its pager widget.
- **[xisback](../xisback)**: the wallpaper daemon. Its windows are `_NET_WM_WINDOW_TYPE_DESKTOP`
  and kiwm keeps them stacked at the very bottom, below every normal window, in creation order --
  required for xisback's crossfade slideshow transitions to look right (a non-compliant WM would
  let the new wallpaper window flash on top of everything for a frame).
- **[xisguard](../xisguard)**: unrelated to window management directly (it's the XNOTIFY
  permission daemon), but part of the same desktop session.
- **[kiconf](../kiconf)**: doesn't configure kiwm directly (kiwm reads its own `kiwm.conf`, see
  below), but is the same "one Qt runtime configurator for the whole session" xisback/xispanel/
  xisguard already plug into.

### Configuration

kiwm reads `$XDG_CONFIG_HOME/kiwm.conf`, falling back to `~/.config/kiwm.conf`. If neither exists,
kiwm writes one with every key set to its built-in default and a comment explaining it, then reads
that -- so `~/.config/kiwm.conf` always has a real, current copy of every option after kiwm has run
once. Lines are `key=value`; `#` starts a comment; unknown keys or malformed lines are skipped with
a warning on stderr, not a hard error. A key you leave out of the file keeps its built-in default.

| Key | Default | Meaning |
|---|---|---|
| `deco_bg` | `#000000` | Fallback titlebar background color (`#rrggbb`), used only when no theme background image loads (see "Theming"). |
| `deco_fg` | `#ffffff` | Fallback title text color, same "only when no theme" scope as `deco_bg`. |
| `hide_deco_on_maximize` | `0` | `1` hides the whole decoration (titlebar + side/bottom border) while a window is maximized, to reclaim every pixel. `0` keeps it. |
| `num_desktops` | `4` | Virtual desktops per output (every output has the same *count*, but its own independent *current* desktop -- see PROTOCOL.md). Clamped to 1..32. |
| `mod_cycle` | `alt` | Modifier (`alt` or `meta`) for Tab/Shift+Tab window cycling, and for left-drag-to-move / right-drag-to-resize from anywhere on a window (not just its titlebar). |
| `mod_control` | `meta` | Modifier (`alt` or `meta`) for window control: drag-to-move/resize (same as `mod_cycle` but a separate binding), Tab/Shift+Tab to cycle the focused output's desktop, Up to maximize/restore. |
| `border_thickness` | `0` | Left/right/bottom decoration border thickness in pixels. `0` means no border at all -- just the titlebar (the original look). |
| `border_color` | `#000000` | Fallback border color, used only when no theme `colors` file overrides it (see "Theming"). |
| `snap_threshold` | `20` | How close (pixels) the pointer must get to an output's *usable* area edge while dragging a window to snap it there -- top edge maximizes, left/right edges fill exactly half the width, Windows7/kwin-style. `0` disables snapping entirely. |
| `focus_follows_mouse` | `0` | `1` raises+focuses a window just by moving the pointer into it ("sloppy focus"). `0` (default) requires an actual click. |
| `theme` | `greenxp` | Theme folder name/path (see "Theming"). Resolved the same way kiwm looks for its own binary-relative files: tried as `../<theme>`, `./<theme>`, and plain `<theme>` (so it works both run from the source tree and installed). |
| `titlebar_layout` | `icon,title,shade,minimize,maximize,close` | Titlebar element order, left to right, comma-separated. See "Titlebar layout" below. |

Two more things affect decoration/theming but aren't `kiwm.conf` keys:

- `$KIWM_DECO_BG` (environment variable): overrides just the theme background image path,
  on top of whatever `theme=` resolves to. Useful for testing a background without touching the
  theme folder itself.
- `$KIWM_HIDE_DECO_ON_MAXIMIZE` (environment variable, `0`/`no` or anything else): a quick
  override *on top of* `hide_deco_on_maximize=`, for testing without editing the config file.

### Titlebar layout

`titlebar_layout=` is a comma-separated list of any of these, in any order, any subset:

- `title` (or `name`) -- the window title. The **only flexible element**: it absorbs whatever
  width the fixed-size ones don't use, wherever it falls in the order. Listing it more than once
  is harmless but pointless -- only the first occurrence gets the flexible width, any further one
  collapses to nothing.
- `icon` -- the window's own icon (`_NET_WM_ICON`), scaled to fit. Drawn blank if the window has
  none.
- `shade` -- toggles shade (collapse to just the titlebar). Scroll-wheel up/down over the titlebar
  does the same thing regardless of whether this button is in the layout at all.
- `minimize`, `maximize` (also serves as "restore" once the window is maximized, same slot),
  `close` -- the usual three.
- `keep_above`, `keep_all_desktops` -- toggle `_NET_WM_STATE_ABOVE`/`_NET_WM_STATE_STICKY` (see
  "Status" above for what "sticky" means in kiwm's per-output desktop model). Shown highlighted
  (theme's hover row, or a darker fallback tint) while active, since there's no dedicated
  "pressed/on" row.

Every element except `title` occupies a fixed-width slot the same size as a button
(`BUTTON_W`, 24px). Hit-testing and hover both read the exact same computed layout drawing does,
so there's no risk of a click landing on the "wrong" element relative to what's actually drawn.

### Theming

kiwm looks for a theme folder (`theme=` in kiwm.conf, default `greenxp`, resolved as
`../<theme>`, `./<theme>`, or plain `<theme>` relative to the current directory) containing any of
these, all optional and independent -- a theme missing some files just falls back to the plain
`kiwm.conf`-configured look for whatever it's missing:

- **`bg.png`** + **`slice`** -- the titlebar background, drawn as a proper 9-slice (same file
  format xispanel's own panel backgrounds use, so one theme folder can serve both): `bg.png` is
  the source image, `slice` is a plain-text sidecar:
  ```
  left=5
  top=5
  right=5
  bottom=5
  ```
  Those four numbers (pixels, measured in the source image) stay unscaled as the four corners;
  everything else stretches to fill whatever's left. A 0-everywhere (or missing) `slice` degrades
  to a plain full-image stretch.
- **`btns.png`** + **`btns.slice`** -- the window-control button sprite sheet. A fixed grid, *not*
  a 9-slice: every cell is the same size (`cell_width=`/`cell_height=` in `btns.slice`, defaults
  to kiwm's own button size if the sidecar is missing). Columns (left to right, fixed order, not
  configurable -- this is about where an icon lives in the image file, unrelated to
  `titlebar_layout=`'s on-screen order): close, maximize, restore, minimize, shade, keep_above,
  keep_all_desktops. Rows (top to bottom): normal, hover, clicked -- "clicked" isn't used yet
  (kiwm fires button actions on press, not release, so there's no separate held-down moment to
  show it during); toggle buttons (keep_above/keep_all_desktops) use the hover row to indicate
  "on" too, in lieu of a dedicated row for that.
- **`colors`** -- per-focus titlebar/border colors, plain `key=value`, `#rrggbb`:
  ```
  bg_active=#3a6ea5
  bg_inactive=#202020
  fg_active=#ffffff
  fg_inactive=#a0a0a0
  border_active=#3a6ea5
  border_inactive=#202020
  ```
  Any key left out (or the whole file missing) falls back to the single `deco_bg`/`deco_fg`/
  `border_color` kiwm.conf values, which in turn produce the plain look: the same titlebar color
  regardless of focus, just a white/black opacity tint layered on top to hint which window is
  active.

### Mouse and keyboard reference

With the defaults (`mod_cycle=alt`, `mod_control=meta`):

- **Click** a window (titlebar or content): focus + raise.
- **Click-drag** a titlebar: move. Drag to a screen edge to snap (see `snap_threshold=` above).
- **Double-click** a titlebar (not on a button): maximize/restore.
- **Scroll** a titlebar: shade/unshade.
- Titlebar buttons: whatever `titlebar_layout=` configures, left to right.
- **Alt+drag** (left button), or **Meta+drag** (any button): move a window from anywhere on it,
  not just its titlebar.
- **Alt+right-drag** or **Meta+right-drag**: resize, from whichever corner of the window is
  nearest wherever you clicked -- the opposite corner stays fixed.
- **Alt+Tab** / **Alt+Shift+Tab**: cycle focus forward/backward among mapped windows on the
  current output's current desktop (plus any sticky ones).
- **Meta+Tab** / **Meta+Shift+Tab**: cycle the focused output's current desktop.
- **Meta+Up**: maximize/restore the focused window.
- **Alt+1**/**2**/**3**/**4**: jump the focused output straight to that desktop.

### Source layout

One `.c`/`.h` pair per concern, all sharing `wm.h` (shared types + `extern KiWM wm`):

- `main.c` -- global `KiWM wm` definition, arg parsing, signal handling, `setup_wm`/`cleanup`,
  the event loop.
- `config.c` -- `kiwm.conf` loader (see "Configuration" above).
- `atoms.c` -- EWMH/ICCCM/custom atom interning.
- `output.c` -- RandR outputs, per-output virtual desktops, dock/panel struts and workarea.
- `decoration.c` -- Cairo/Imlib2 frame painting, theme loading, titlebar layout math.
- `ewmh.c` -- EWMH property bookkeeping on clients (client list, active window, state/desktop/
  output, title, frame extents).
- `client.c` -- manage/unmanage, focus/stacking, move/resize/maximize/minimize/shade/keep-above/
  sticky transitions.
- `events.c` -- X event dispatch, delegating actual state changes to the modules above.
- `selection.c` -- `--replace` (ICCCM manager-selection handoff).

`kiwm-gpt.c` is an earlier, single-file GPT-authored attempt (single global workspace, no RandR,
no theming) kept only as historical reference -- not built by the Makefile, not maintained.
