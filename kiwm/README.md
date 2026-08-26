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
- Themed on-screen overlays (`osd_enabled=`, default on) for both window cycling and desktop
  switching -- see "On-screen overlays (OSD)" below.
- Virtual desktops tracked **independently per output** (not one global workspace number) --
  see [PROTOCOL.md](PROTOCOL.md).
- Already-open windows are picked up at startup, not just windows mapped afterward -- *with the
  state they were already in*: `_NET_WM_STATE` is read off each window as it's adopted, so a window
  the previous WM left maximized, fullscreen, shaded, above/below, sticky or minimized comes up that
  way under kiwm too, instead of "floating, but coincidentally the exact size of a maximized window"
  (which made unmaximizing appear to do nothing). Reading that property at manage time is also what
  EWMH says a WM must do for an app that asks for an initial state *before* mapping -- a client
  message can only reach an already-managed window, so setting the property is the only way to ask
  -- which is how e.g. VirtualBox's VM window requests to come up fullscreen. There's no way to
  recover the *pre*-maximize floating geometry across a WM switch (EWMH has no property for it; the
  old WM held it in memory and took it along), so a restore falls back to a centered two thirds of
  the workarea.
- Clients that ask for no decoration are honored: `_MOTIF_WM_HINTS` with `decorations=0` (what Qt's
  `FramelessWindowHint`, GTK's `gtk_window_set_decorated(false)` and SDL borderless windows all
  actually put on the wire) and KDE's `_KDE_NET_WM_WINDOW_TYPE_OVERRIDE` (kwin's "noBorder", set by
  VirtualBox's VM window among others). Still fully managed -- framed, focusable, in the taskbar,
  tiles and maximizes normally -- the frame just has no titlebar or border, so it ends up exactly
  the size of the content instead of stacking kiwm's chrome on top of the app's own.
- `--replace`: proper ICCCM manager-selection handoff, and kiwm itself can later be `--replace`d
  cleanly by something else.
- Frames also select `SubstructureRedirect` (not just root): a client repositioning *itself* well
  after being mapped (many toolkits do this once, to restore a remembered window position/size,
  unaware it's reparented at all -- ICCCM requires that transparency) gets redirected back through
  kiwm like any other geometry request, instead of applying directly against its real parent (the
  frame) with no translation at all -- which is what used to shove a reopened window's *content* off
  inside its own frame by roughly however far the window used to be from `(0,0)`, cut off in a
  corner: the app's intended *absolute screen* position was landing as a raw frame-relative offset.
- Graceful shutdown on SIGTERM/SIGINT: every managed window is reparented back to root before
  kiwm exits, so windows survive a kill instead of vanishing with it.
- Dock/panel awareness: `_NET_WM_STRUT`/`_NET_WM_STRUT_PARTIAL`, correctly attributed per output
  (a panel on one monitor doesn't eat into a different monitor's usable area), feeding
  `_NET_WORKAREA` and maximize.
- Window-type-aware framing: besides `_NET_WM_WINDOW_TYPE_DOCK`/`_DESKTOP`/`_TOOLBAR`/`_MENU`,
  `_POPUP_MENU`/`_DROPDOWN_MENU`/`_TOOLTIP`/`_NOTIFICATION`/`_COMBO`/`_DND`/`_SPLASH` and KDE's own
  non-standard `_KDE_NET_WM_WINDOW_TYPE_APPLET_POPUP` (which Plasma sets *instead of* a standard
  type on every applet popup -- the notification popup, clipboard, volume, battery...) are also never
  framed/decorated or repositioned -- mapped exactly as the app placed them, geometry untouched.
  Checked across *every* type a window lists (not just the first), so a specific type followed by
  `_NORMAL` as a generic fallback still gets recognized. Matters most for a desktop environment's
  own popups (a KDE Plasma session's application launcher, applet popups, panel tooltips) once
  they're talking to a WM that isn't their own (KWin's private handling for these doesn't apply) --
  an unrecognized type used to fall through to full framing, which is what was giving them a
  titlebar they were never supposed to have, and (for tooltips especially) is what let kiwm's normal
  per-output client positioning logic get involved in placing them at all, versus just leaving them
  exactly where the app put them.
- Magnetic edge snapping while moving *or* resizing a window: an edge within `magnet_threshold`
  (default 10px) of another window's edge (decoration included), a dock/panel/taskbar's edge on
  the *same output* (one on a different monitor is ignored), or the screen edge snaps flush
  against it, gap-free -- independent per axis, and distinct from the tiling snap below. Other
  Clients count as candidates regardless of which output they're on (so two windows on
  neighboring monitors can still snap to each other).
- Optional (`link_resize_neighbors=`, default off): while resizing, whatever's touching the edge
  being dragged (within 1px, same output) gets resized right along with it, oppositely, so both
  stay touching -- shrink one and its neighbor grows by the same amount, and vice versa. Resizing
  the shared edge of two windows half-snapped (see the tiling snap below) to opposite sides of the
  screen resizes both in place, still half-snapped, instead of detiling back to their pre-snap
  floating size the way every other resize/move on a snapped window still does -- *moving* a
  half-snapped window (or resizing an edge that isn't the shared one) detiles it as always.
- Windows7/kwin-style edge-drag snapping (drag a titlebar to a screen edge to maximize/half-tile),
  corner-relative resize, double-click-titlebar-to-maximize, scroll-wheel shade. Move/resize show
  the matching cursor from the user's actual Xcursor theme (via libxcb-cursor, same lookup rules as
  libXcursor), falling back to the plain core font cursor if that can't be set up at all.
- Window states beyond the basics: shade (`_NET_WM_STATE_SHADED`), keep-above/keep-below
  (`_NET_WM_STATE_ABOVE`/`_BELOW`), sticky/keep-on-all-desktops (`_NET_WM_STATE_STICKY`, meaning
  "visible regardless of this window's own output's current desktop" -- see PROTOCOL.md),
  fullscreen (`_NET_WM_STATE_FULLSCREEN` -- covers the whole output including any docks/panels,
  decoration unconditionally hidden, restores back to whatever floating/maximized/snapped state
  the window was in beforehand).
- A real multi-layer stacking model (`below < normal < above`, a client's layer derived from its
  state above -- fullscreen has no layer of its own, it shares `normal` with every plain window, so
  it's only ever on top *because it's focused*: Alt+Tab-ing away raises the newly-focused window
  above it like any other focus change, instead of a fullscreen window being unconditionally pinned
  above every plain window regardless of focus): `client.c`'s `restack_all()` rebuilds the whole X
  stacking order from it on every change, preserving each client's relative order within its own
  layer instead of just re-raising keep-above windows on top of whatever's currently there.
- ICCCM `WM_NORMAL_HINTS`' minimum size (`PMinSize`) is honored wherever a window's size gets
  clamped (initial map, interactive resize, maximize, edge-snap, `_NET_MOVERESIZE`-style
  configure requests) -- floored to kiwm's own absolute minimum so a client that sets a tiny or no
  hint at all can still never be resized down to something unusably small.
- A small theme system (background image, button sprite sheet, per-focus colors, corner rounding
  via the XCB SHAPE extension -- no compositor needed) with a configurable titlebar element order
  -- see "Theming" below.
- Enough EWMH/ICCCM for a taskbar (xispanel's tasklist widget) to list/activate/close/minimize/
  maximize windows: `_NET_CLIENT_LIST(_STACKING)`, `_NET_ACTIVE_WINDOW`, `_NET_CLOSE_WINDOW`,
  `_NET_WM_STATE`, ICCCM `WM_STATE`, `_NET_WM_DESKTOP`, `_NET_SUPPORTING_WM_CHECK`,
  `_NET_WORKAREA`, `_NET_FRAME_EXTENTS`, `_NET_WM_ICON`.

Not implemented yet: `kicomp` compositor (no client-side compositing at all), resizing by grabbing
the window's own edge/corner with no modifier held (only mod+right-click resize exists so far),
global menu.

### Dependencies

- libxcb, libxcb-randr, libxcb-shape, libxcb-cursor, libxcb-icccm
- Cairo with the `cairo-xcb` backend
- Imlib2
- Pango + PangoCairo (titlebar text: per-glyph font fallback across scripts and ellipsizing)

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
| `mod_cycle` | `alt` | Modifier (`alt` or `meta`) for left-drag-to-move / right-drag-to-resize from anywhere on a window (not just its titlebar). Also what `ModCycle` resolves to in the `key_*` shortcuts below, which is how the window-switching defaults follow it. |
| `mod_control` | `meta` | Modifier (`alt` or `meta`) for window control: drag-to-move/resize (same as `mod_cycle` but a separate binding). Also what `ModControl` resolves to in the `key_*` shortcuts, which is how the desktop-switch/maximize/minimize/tile defaults follow it. |
| `border_thickness` | `0` | Left/right/bottom decoration border thickness in pixels. `0` means no border at all -- just the titlebar (the original look). |
| `border_color` | `#000000` | Fallback border color, used only when no theme `colors` file overrides it (see "Theming"). |
| `snap_threshold` | `20` | How close (pixels) the pointer must get to an output's *usable* area edge while dragging a window to snap it there -- top edge maximizes, left/right edges fill exactly half the width, Windows7/kwin-style. `0` disables snapping entirely. |
| `magnet_threshold` | `10` | How close (pixels) a window's *edge* (not the pointer -- the frame, decoration included), while being moved or resized, must get to another window's edge, a same-output dock/panel/taskbar's edge, or the screen edge before it snaps flush against it, gap-free -- a much smaller, purely cosmetic nudge than `snap_threshold`'s tiling snap above. `0` disables it. |
| `link_resize_neighbors` | `0` | `1` makes resizing also resize whatever's touching (within 1px) the edge being dragged, oppositely, so both stay touching -- same output only. `0` (default) leaves resizing exactly as before. |
| `focus_follows_mouse` | `0` | `1` raises+focuses a window just by moving the pointer into it ("sloppy focus"). `0` (default) requires an actual click. |
| `osd_enabled` | `1` | `1` (default) shows a themed overlay while holding Alt+Tab/Meta+Tab, only switching on release -- see "On-screen overlays (OSD)" below. `0` reverts to switching immediately on every Tab press, no overlay. |
| `osd_live_preview` | `0` | `1` applies every Tab step live (raise/focus, or switch desktop) instead of only on release -- Escape then reverts to whatever was active before the hold started. `0` (default) leaves everything untouched until release. Ignored when `osd_enabled=0`. |
| `osd_output_follows_pointer` | `0` | `1` opens an overlay on whichever output the pointer is on (polled once when the hold starts), instead of the currently focused window's output (`0`, default; falls back to the pointer's output only when nothing is focused). Not the same as `focus_follows_mouse=` -- only decides which screen Alt+Tab/Meta+Tab themselves act on. |
| `theme` | `greenxp` | Theme folder name/path (see "Theming"). Resolved the same way kiwm looks for its own binary-relative files: tried as `../<theme>`, `./<theme>`, and plain `<theme>` (so it works both run from the source tree and installed). |
| `titlebar_layout` | `icon,title,shade,minimize,maximize,close` | Titlebar element order, left to right, comma-separated. See "Titlebar layout" below. |
| `key_*` | see below | Global keyboard shortcuts, one key per action (`key_minimize=Meta+Down`, ...). See "Keyboard shortcuts" below for the full list, the syntax, and how to unbind one. |

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

  Two more keys in the same `colors` file control corner rounding (via the XCB SHAPE extension --
  no compositor needed, so this works even without `kicomp`):
  ```
  border_radius=8
  round_maximized=1
  ```
  `border_radius=` is 1, 2, or 4 numbers (pixels): one value rounds all four corners the same;
  two values are "top corners, bottom corners"; four are `top-left,top-right,bottom-right,
  bottom-left` (CSS `border-radius` order). Default `0` (no key, or no `colors` file at all) means
  square corners, unchanged from before this existed. `round_maximized=` (default `0`) squares a
  maximized window's corners off -- set to `1` to keep rounding them too. A window that exactly
  fills its whole output (also what a future real fullscreen state would look like) is **never**
  rounded either way, regardless of these settings -- rounding the very corners of the screen
  itself would just
  show the desktop background poking through.

  Three more keys control the title text, rendered via Pango (per-glyph font fallback, so titles
  in scripts the default font doesn't cover -- CJK, Cyrillic, Arabic, etc. -- still show up instead
  of leaving blank gaps, plus proper `...` ellipsizing instead of a hard clip):
  ```
  font=sans-serif
  font_size=12.5
  title_center=0
  ```
  `font=` is any Fontconfig family name; `font_size=` is in pixels. Both default to the values
  above (kiwm's original hardcoded look) if left out or no `colors` file exists. `title_center=`
  (default `0`) centers the title within its slot instead of left-aligned with an 8px pad -- the
  title element is always the greedy one in `titlebar_layout=` (it soaks up whatever width isn't
  used by the other elements), so this is just a text alignment choice, no separate spacer element
  needed either way.

### Mouse and keyboard reference

Every keyboard shortcut below is rebindable -- see "Keyboard shortcuts". The mouse gestures aren't
separately bindable; they follow `mod_cycle=`/`mod_control=` directly. With the defaults
(`mod_cycle=alt`, `mod_control=meta`):

- **Click** a window (titlebar or content): focus + raise.
- **Click-drag** a titlebar: move. Drag to a screen edge to snap (see `snap_threshold=` above).
- **Double-click** a titlebar (not on a button): maximize/restore.
- **Scroll** a titlebar: shade/unshade.
- Titlebar buttons: whatever `titlebar_layout=` configures, left to right.
- **Alt+drag** (left button), or **Meta+drag** (any button): move a window from anywhere on it,
  not just its titlebar.
- **Alt+right-drag** or **Meta+right-drag**: resize, from whichever corner of the window is
  nearest wherever you clicked -- the opposite corner stays fixed.
- **Alt+Tab** / **Alt+Shift+Tab**: hold Alt, tap Tab/Shift+Tab to step forward/backward through
  mapped windows on the current output's current desktop (plus any sticky ones) -- releasing Alt
  commits whichever is highlighted (see "On-screen overlays (OSD)" below; `osd_enabled=0` switches
  immediately on every tap instead, with no overlay).
- **Meta+Tab** / **Meta+Shift+Tab**: same idea, for the focused output's current desktop.
- **Escape**: while either overlay is open, cancel without switching.
- **Meta+Up**: maximize/restore the focused window.
- **Meta+Down**: minimize the focused window.
- **Meta+Left** / **Meta+Right**: tile the focused window to the left/right half of its output's
  usable area -- the same geometry a drag to that screen edge produces, without the drag. Pressing
  it again for the side the window is *already* tiled to restores it, so one key both tiles and
  untiles.
- **Alt+1**/**2**/**3**/**4**: jump the focused output straight to that desktop (immediate, no
  overlay -- a direct-select shortcut, not a cycle).

### Keyboard shortcuts

All of kiwm's global keyboard shortcuts live in `kiwm.conf` as `key_*` keys, and a freshly
generated config lists every one this build has, with its default:

```
key_window_next=ModCycle+Tab
key_window_prev=ModCycle+Shift+Tab
key_desktop_next=ModControl+Tab
key_desktop_prev=ModControl+Shift+Tab
key_maximize=ModControl+Up
key_minimize=ModControl+Down
key_tile_left=ModControl+Left
key_tile_right=ModControl+Right
key_fullscreen=
key_shade=
key_keep_above=
key_sticky=
key_close=
key_desktop_1=
key_desktop_2=            # ...through key_desktop_8, unbound by default
```

- Syntax is `Mod+Mod+Key`, case-insensitive: `Meta+Down`, `alt+shift+Tab`, `Ctrl+Alt+F1`.
- Modifiers: `Alt`, `Meta` (= `Super`/`Win`), `Ctrl`, `Shift`, plus `ModCycle`/`ModControl`, which
  resolve to whatever `mod_cycle=`/`mod_control=` are set to. The defaults use the symbolic pair on
  purpose, so setting `mod_cycle=meta` moves every default cycling shortcut along with it -- exactly
  as it behaved when these were hardcoded.
- Keys are named like their X keysyms (`Tab`, `Up`, `Down`, `Left`, `Right`, `Escape`, `Return`,
  `space`, `Home`, `End`, `PageUp`, `PageDown`, `Delete`, `F1`-`F12`), a single printable character
  (`a`, `7`, `/`), or a raw `0x<hex>` keysym for anything else.
- An **empty value** leaves that action unbound (and ungrabbed) -- that's what the actions with no
  default above are. A line that's present but empty wins over the built-in default, so a shortcut
  can be turned off, not just moved.
- Every shortcut is grabbed with and without NumLock/CapsLock, so none of them silently stop
  working with either lock key on.
- **Escape** isn't in the table: it's the fixed cancel key for whatever hold is in progress (the
  overlays below), never grabbed and not rebindable.

### On-screen overlays (OSD)

With `osd_enabled=1` (the default), holding `mod_cycle`/`mod_control` (Alt/Meta by default) and
tapping Tab opens a themed overlay -- same background/border colors and corner radius as the
window decoration (see "Theming" above), drawn with the XCB SHAPE extension, no compositor needed
-- centered on the output that has the currently focused window (or whichever output the pointer
is on, if nothing's focused), or always the pointer's output with `osd_output_follows_pointer=1`
(polled once when the hold starts, then fixed for that hold -- deliberately not the same thing as
`focus_follows_mouse=`, since this never changes what receives keyboard input, only which screen
Alt+Tab/Meta+Tab themselves act on and list -- kept as a separate setting from any future
compositor effect on the same two actions, e.g. `kicomp` eventually animating the switch itself,
since "which screen this affects" stays a meaningful question independent of whether there's an
overlay/animation to show at all):

- **Alt+Tab**: a simple vertical list of eligible windows (icon + title), the pending selection
  highlighted. Each further Tab/Shift+Tab while Alt stays held moves the highlight.
- **Meta+Tab**: a pager-style grid of the current output's desktops (squares proportional to the
  output's real resolution, like xispanel's pager widget), the pending selection highlighted.
- By default (`osd_live_preview=0`), nothing actually changes until the modifier is released --
  browse freely, decide, then let go. With `osd_live_preview=1`, every step already applies live
  (raises+focuses the highlighted window, or switches to the highlighted desktop) as you move
  through it, same as most desktops' Alt+Tab -- **Escape** then reverts back to whatever was
  actually focused/current *before* the hold started, not just "cancels" a no-op.
- A window that closes while the Alt+Tab list is open (e.g. a crash) is quietly dropped from the
  list in place, selection re-clamped -- it's never focusable, and if it was the only entry left
  the overlay just closes. If it was also the window `osd_live_preview`'s Escape-revert was going
  to restore, that revert is dropped too (there's nothing left to revert to).

Internally, kiwm actively grabs the keyboard (`xcb_grab_keyboard()`) for as long as an overlay is
open -- the only reliable way to see the modifier key's own release regardless of which client (if
any) has input focus; a plain `xcb_grab_key()` binding (what every other kiwm shortcut uses) can't
by itself. Committing is decided by *live keyboard state*, not by matching the released key against
a specific hardcoded keycode: every KeyRelease while an overlay is open re-queries whether
`mod_cycle`/`mod_control`'s bit is still set at all (`xcb_query_pointer()`'s modifier mask) and only
commits once it's actually gone. Matching one fixed keycode (e.g. just `Alt_L`) instead would miss a
layout where the modifier lives on a different/second physical key, or misfire on a modifier key's
own X autorepeat -- either way leaving the keyboard grab stuck engaged (every keystroke system-wide
silently swallowed by an OSD nobody can see) until something else forced it shut, which is exactly
the "100% CPU, no window will open" wedge that shipped in this feature's first cut. The window list
is built behind a small `TabBoxOps` vtable (`osd.h`/`osd.c`) -- kwin calls the same idea a "tabbox"
-- so a different presentation (a thumbnail grid, cover-flow, ...) can be swapped in later by
writing a new `TabBoxOps` and pointing one variable at it, with no changes to the hold/release
mechanics or eligibility rules. Only one implementation exists today: `simple_list_tabbox_ops`, the
plain list above. The desktop grid isn't behind such a vtable -- it's a single fixed presentation,
not asked to be swappable.

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
- `keybind.c` -- configurable global keyboard shortcuts: one table holding every action, its
  `kiwm.conf` key, its default binding and its generated documentation, plus the spec parser, the
  root-window grabs and the dispatch (see "Keyboard shortcuts" above).
- `selection.c` -- `--replace` (ICCCM manager-selection handoff).
- `osd.c` -- Alt+Tab/Meta+Tab on-screen overlays (see "On-screen overlays (OSD)" above).

`kiwm-gpt.c` is an earlier, single-file GPT-authored attempt (single global workspace, no RandR,
no theming) kept only as historical reference -- not built by the Makefile, not maintained.
