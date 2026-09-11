# kiwm

**kiwm** is the stacking window manager for **KiDesktop** -- plain XCB (no Xlib), Cairo + Imlib2
for decoration, RandR for multi-monitor. KiDesktop is built with XiS/XLibre in mind but isn't
exclusive to it -- it targets X11 generally, and kiwm works the same standalone on any X server
(no compositor required); an optional compositor (`kicomp`) is planned alongside it but never a
dependency. See [kiwm-kicomp-projeto.md](kiwm-kicomp-projeto.md) for the full architecture/phased
plan this was built from, and [PROTOCOL.md](PROTOCOL.md) for the one custom (non-EWMH) protocol it
exposes.

### Status

Complete enough to be the window manager this desktop is run on day to day. What works:

- map/unmap/move/resize/maximize/minimize/close, click-to-focus (or optional
  focus-follows-mouse), Alt-Tab-style window cycling.
- Maximization is tracked **per axis**, as EWMH has always defined it: `_NET_WM_STATE_MAXIMIZED_VERT`
  and `_NET_WM_STATE_MAXIMIZED_HORZ` are two independent states, published independently, and a
  client message naming only one of them moves only that axis. A window the previous WM left
  maximized in a single direction is adopted that way too. "Maximized" with no qualifier means both
  axes; the single-axis ones are on the maximize button's right and middle click (see "Mouse and
  keyboard reference").
- Every color kiwm reads -- the theme's `colors` file and `kiwm.conf`'s own `deco_bg=`/`deco_fg=`/
  `border_color=` -- takes `#rrggbb` **or** `#rrggbbaa`. The alpha is real on an ARGB frame (a client
  with a 32-bit visual gets a 32-bit frame, whose pixmap starts fully transparent), which is what
  makes a translucent titlebar possible once a compositor is running; without one the X server
  ignores it, as it always has, and a root-depth frame has no alpha channel to ignore. The title
  text is always drawn fully opaque whatever alpha `fg_active`/`fg_inactive` carry -- that alpha is
  about how translucent the titlebar is, and the window's name has to stay readable over whatever
  shows through it.
- Themed on-screen overlays (`osd_enabled=`, default on) for both window cycling and desktop
  switching -- see "On-screen overlays (OSD)" below. The window list includes minimized windows
  (dimmed, as a taskbar shows them), and committing to one restores it. A minimized window is still
  outlined where it was when it went away -- kiwm never loses that geometry, since minimizing only
  unmaps the frame -- and that same rectangle is published as `_KIWM_MINIMIZED_GEOMETRY` (see
  [PROTOCOL.md](PROTOCOL.md)) for anything outside kiwm that wants it, a compositor animating the
  minimize/restore above all.
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
- A window a client hides and shows again comes back *with its contents*, and on top. Qt's `hide()`
  unmaps the client's own window, and the `MapRequest` that shows it again is denied until the WM
  maps that window -- the whole point of the frame's `SubstructureRedirect`. Mapping only the frame
  (which is all kiwm did) brings back a decorated hole showing whatever is behind it: OpenSnitch's
  prompt after the first "OK", krunner after its first invocation. Moving, resizing or maximizing
  doesn't help, since there's nothing there to repaint -- only shade+unshade, which unmaps and
  remaps the content window on purpose, did. The frame is also raised, so a window an app just
  chose to show again doesn't come back behind whatever has been focused since it last hid.
- A client unmapping its own window is treated as ICCCM says it must be: a **withdrawal**. The
  window stops being managed entirely (frame destroyed, `WM_STATE`/`_NET_WM_STATE`/`_NET_WM_DESKTOP`
  removed with it, per ICCCM and EWMH), instead of kiwm keeping a hidden Client around forever --
  which is what left every OpenSnitch prompt ever answered sitting in the taskbar, since the window
  stayed in `_NET_CLIENT_LIST`. Dropping the stale `WM_STATE` matters for more than tidiness:
  startup adoption deliberately picks up an unmapped window that still carries one (that's how a
  window minimized under the previous WM survives a `--replace`), so leaving it behind would
  resurrect every withdrawn window as a hidden client on the next start. Showing the window again
  then takes the ordinary `MapRequest` path as a brand-new window, which is also what makes it come
  back focused. kiwm's own ways of hiding a window are unaffected: minimizing and switching desktops
  unmap the *frame*, leaving the client window mapped (just not viewable), and generate no
  `UnmapNotify` for it at all.
- `WM_TAKE_FOCUS` is sent to clients that list it in `WM_PROTOCOLS`. `SetInputFocus` alone is only
  half of handing over the keyboard for such a client: it also has to be *told*, so it can route the
  focus internally and update its own idea of which window is active. Every Qt/KDE window asks for
  this; krunner is where skipping it shows, opening with a dead input field even though X focus is
  already on it. ICCCM requires a real timestamp in that message (never `CurrentTime`), so kiwm
  keeps the newest one the server has handed it on any event that carries one.
- `_NET_WM_MOVERESIZE`: a client can ask kiwm to take over a move or resize *it* decided the user
  started, which is how a window gets dragged by empty space inside it, with no titlebar involved.
  Qt's Breeze/Oxygen styles send it from blank areas of toolbars and dialogs, GTK headerbar apps
  from the headerbar, and windows that draw their own chrome instead of taking a decoration (the
  Steam client) from wherever they consider draggable. Without it those drags do nothing at all,
  since the app is deliberately *not* moving its own window -- it's waiting for the WM to. kiwm
  resizes from a corner only, so the four edge directions fall back to the nearest-corner rule a
  plain drag uses on the axis they don't name, and the keyboard variants are treated as their
  pointer equivalents.
- A maximized or half-tiled window doesn't leave that state the moment it's clicked. Dragging one
  by the titlebar (or with a modifier from anywhere on it) used to restore it on contact, which made
  every click on a maximized titlebar a gamble -- the click is usually aimed at a button, or at
  nothing. Now the window stays put until the pointer has actually travelled a titlebar's height
  from where it was pressed, and only then detiles, under the cursor, with the drag re-anchored
  there as if it had started at that point. A *resize* still detiles immediately: it's unambiguous
  about wanting a different size.
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
- Graceful shutdown on SIGTERM/SIGINT/SIGHUP: every managed window is reparented back to root
  before kiwm exits, so windows survive a kill instead of vanishing with it. SIGHUP matters because
  kiwm is normally started from a terminal, and closing that terminal hangs up its process group --
  left at the default disposition that's an immediate death with no cleanup at all.
- ...and windows survive kiwm **not** exiting gracefully, too, which is what the X **save-set** is
  for: every client window is added to it when kiwm reparents the window into its frame. Destroying
  a window destroys its children, and the server destroys every window a client created when that
  client's connection drops -- so a kiwm that dies without unwinding its frames (a crash, a
  `SIGKILL`, an X error) took every window reparented inside them along: the whole session's apps
  quit at once, everything in the taskbar gone, only the never-framed panels and desktop left
  standing. The save-set makes the server reparent those windows back to the root and remap them at
  close-down instead. One request per window, and the orderly `cleanup()` path drops each window
  from the save-set as it hands it back itself.
- Dock/panel awareness: `_NET_WM_STRUT`/`_NET_WM_STRUT_PARTIAL`, correctly attributed per output
  (a panel on one monitor doesn't eat into a different monitor's usable area), feeding
  `_NET_WORKAREA` and maximize. The usable area is *re-applied* whenever it changes: every
  maximized, half-tiled or fullscreen window is resized to the new one, so a panel starting,
  quitting, moving or changing its strut doesn't leave maximized windows sized for the old layout.
  That's also what makes adoption at startup come out right -- windows are framed in
  `XQueryTree()` order, and a panel is just another window in that list, so a window adopted
  maximized before the panel it shares a screen with had computed its size against a workarea with
  no panel in it yet (covering the taskbar); one refit pass once every dock is known fixes it.
- Multi-monitor EWMH that doesn't lie about the second screen. `_NET_WORKAREA` has room for exactly
  one rectangle for the whole desktop, and publishing the *primary output's* usable area there (what
  kiwm did, and what KWin does) is an actively harmful lie: a client that dutifully constrains its
  own popups to that rectangle drags every popup belonging to a window on any other output onto the
  primary one -- measured on a two-monitor session, a panel tooltip for the second screen landing at
  `x=0`. kiwm now publishes the bounding box of every output's usable area (identical to before on a
  single monitor), which at least *contains* every real work area; the exact per-output usable area
  is still what maximize uses and is still exposed losslessly through the `_KIWM_*` protocol.
  `_NET_DESKTOP_GEOMETRY`/`_NET_DESKTOP_VIEWPORT` are published too (they weren't at all, so clients
  read whatever the previous WM left behind, or nothing on a fresh session), and
  `_NET_WM_FULL_PLACEMENT` is advertised -- per EWMH that tells clients to stop constraining their
  own popups to a screen, which is exactly what kiwm wants for the windows it never moves.
- Windows uncovered by a fullscreen window that loses focus are asked to repaint. Dropping out of
  the active-fullscreen layer uncovers whatever was under it, but X only sends `Expose` for regions
  it considers newly visible -- a panel that was fully covered and is now merely *above* the same
  window gets nothing, and is left displaying the pixels the fullscreen window painted over it (a
  game's picture stuck on the panels). `ClearArea` with `exposures` asks for those events
  explicitly. The switcher overlay also repaints on `Expose` now, for the same reason.
- ...and asked *again* 150ms and 500ms later, because one round isn't enough. Without a compositor
  the X server page-flips a window that covers a whole output and is topmost and unobscured
  straight to the scanout (DRI3/Present): while that's in effect the server's own screen pixmap is
  stale, so everything other clients draw goes into a buffer nobody is looking at. When the window
  stops qualifying -- which is the instant kiwm drops it out of the active-fullscreen layer -- the
  server "unflips" by copying that last flipped frame, the video frame, into the screen pixmap, and
  *that* is what wipes the repaint the immediate `ClearArea` round just triggered. Hence the later
  rounds, once the unflip has certainly settled. Verified live against the actual symptom (SMPlayer
  fullscreen on one output, click a window on the other): an `xrefresh` over the same region right
  away leaves the panel corrupted, the same `xrefresh` a second later restores it; before the fix
  4 of 5 focus switches left the panel holding video pixels, after it 5 of 5 were clean.
  `force_unflip=` in kiwm.conf picks whether those two extra rounds are sent: `auto` (the default)
  sends them only when nothing else is repairing the screen -- no compositor owns
  `_NET_WM_CM_S<n>`, and the server has `Present`, i.e. it can page-flip at all -- `always` sends
  them unconditionally, `never` sends only the immediate round and trusts the server's single
  unflip. (Whether the server flips each *CRTC* separately, the driver option that makes this bite
  with a fullscreen window on one output of several, is not something a client can query, so
  "the server can flip" is as far as the probe goes.)
- Non-rectangular (shaped) client windows: a client's own SHAPE (bounding *and* input) is forwarded
  onto the frame kiwm reparents it into -- which the X server is what actually clips against once
  the client is a child of that frame, so without it the frame stays a solid rectangle covering,
  and swallowing every click over, whatever the client carved away. `xeyes` is the textbook case;
  the one that matters in practice is VirtualBox's fullscreen mini-toolbar, a screen-sized window
  whose shape is just the little bar at the top, floating over the VM window. `ShapeNotify` is
  tracked too, so a shape set or changed after mapping is picked up, not just the one present at
  manage time. When a client is shaped, kiwm's own corner rounding steps aside (the client is
  already saying exactly what silhouette it wants) but the decoration's rectangles are unioned back
  in, so a shaped window still gets a whole titlebar.
- Windows that belong together stay together, through two independent relationships, because real
  applications use both:
    - ICCCM `WM_TRANSIENT_FOR` -- a transient takes its parent's layer and is ordered above it.
    - The ICCCM **window group** (`WM_HINTS`' `window_group`, falling back to `WM_CLIENT_LEADER`) --
      for windows with no transient relationship at all. Whichever member of a group has focus
      lifts the whole group into the active-fullscreen layer if any member is fullscreen, and a
      member marked `_NET_WM_STATE_SKIP_TASKBAR` (the client's own "I'm not a window you switch
      to") is kept above the group's ordinary windows. VirtualBox's fullscreen mini-toolbar needs
      exactly this and nothing less: same group as the VM window, skip-taskbar, *no*
      `WM_TRANSIENT_FOR`, and it never restacks itself -- so a WM that only understands transients
      loses it behind the VM the moment the VM is clicked.
  Skip-taskbar windows are also left out of the window switcher, where an entry that isn't a
  window you can meaningfully switch to is just noise.
- ICCCM `WM_TRANSIENT_FOR` is honored as a stacking relationship: a transient window takes its
  parent's *layer* (not its own) and is then ordered above it within that layer, so a window and
  its dialogs/toolbars always travel together and focusing the parent can't bury its own dialog.
  Sharing the layer is the part that matters: a fullscreen, focused parent moves to a layer of its
  own (see below), and a transient left behind in `normal` could never be ordered back above it.
  The property is re-read on `PropertyNotify`, not just at manage time -- toolkits don't all set it
  before mapping, which was the whole difference between VirtualBox's mini-toolbar working when
  kiwm adopted an already-running VM and not working when the VM started under a running kiwm.
  The motivating case throughout: that toolbar is only ever "on top" because a WM keeps transients
  there, VirtualBox never restacks it itself.
- Stacking requests from clients (`XRaiseWindow`/`XLowerWindow`, i.e. a `ConfigureRequest` carrying
  a stack mode) are applied to the frame and then run through the layer model, instead of being
  silently dropped as they used to be. The requested *sibling* is deliberately ignored: it names
  the client's would-be siblings, which under a reparenting WM aren't the frame's.
- Adopted windows keep their map state. A `MapRequest` means "show me", but a window kiwm merely
  *finds* at startup is left exactly as it is -- an unmapped one is unmapped on purpose (a
  minimized window, or a hidden-away popup an app keeps around between uses: krunner, Plasma applet
  popups, VirtualBox's auto-hidden mini-toolbar). Two things are needed for that to actually hold:
  ICCCM `WM_STATE` (not the live map state) is what says "minimized", since a WM that hides a
  window by unmapping its *frame* leaves the client window itself mapped and the outgoing WM's
  reparent-back-to-root makes it genuinely viewable again moments before kiwm looks; and the
  automatic re-map the X server performs at the end of every reparent has to be swallowed, or the
  ordinary "the app is showing this window" path maps the frame right back. Without those, a WM
  switch sprayed the screen with every hidden window an app owned, at whatever stale position it
  was last left at.
- Unframed-but-interactive popups get the keyboard. The popup/menu window types kiwm doesn't frame
  are still not override-redirect -- they're ordinary top-levels whose app expects the WM to focus
  them -- so a Plasma launcher that opened, drew and took clicks would silently swallow every
  keystroke typed into its search field. Applet popups, popup/dropdown menus and combos are focused
  when mapped and hand the keyboard back when they go away; tooltips, notifications, splashes, drag
  icons and docks deliberately never take focus.
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
- A real multi-layer stacking model -- `below < normal < dock < above < active-fullscreen < osd` --
  which `client.c`'s `restack_all()` rebuilds the whole X stacking order from on every change,
  preserving each window's relative order within its own layer instead of just re-raising
  keep-above windows on top of whatever's currently there. The first four follow EWMH's suggested
  order; the last two are the interesting ones:
    - **active-fullscreen**: a fullscreen window goes above everything, docks included, but only
      while it (or one of its transients) is focused -- kwin's "active layer", and what
      mutter/metacity do too. That's what lets a fullscreen video or VM cover the panel while
      you're using it, and lets Alt+Tab bring any other window, or the panel, straight back over
      it. The two alternatives are both wrong in practice: pinning fullscreen on top forever, or
      leaving it in `normal` where the panel floats over a fullscreen VM.
    - **osd**: kiwm's own surfaces (the switcher overlays today, whatever `kicomp` draws later).
      Nothing a client can ask for reaches this layer, which is the point -- it's the one layer
      guaranteed to sit above even an active fullscreen window, so kiwm's own UI can never end up
      behind the window it's offering to switch away from.
  Dock/panel windows are never Clients, but `restack_all()` still places them (`LAYER_DOCK`):
  where a panel sits relative to normal, keep-above and fullscreen windows is exactly the kind of
  question only the WM can answer, and before this it simply sat wherever X had left it -- which
  meant permanently on top of everything.
- ICCCM `win_gravity` decides where the frame goes. `StaticGravity` -- which is what essentially
  every Qt/GTK window asks for -- means "my *content* goes where I asked, put your titlebar above
  it", so the frame is placed a decoration's worth up and left; anything else falls back to the
  ICCCM default (`NorthWest`: the frame goes where the client asked and the content lands below the
  titlebar). Ignoring this is invisible on a freshly mapped window but walks every window a
  titlebar's height down the screen on each WM handoff, since adoption re-frames windows that are
  already on screen -- measured at +54px per switch on a KWin session. The same rule applies to a
  later `ConfigureRequest` from the client. A static-gravity frame is never pushed above its
  output's usable area, so the titlebar can't end up off-screen and undraggable.
- Actions a window says it doesn't support are hidden *and* refused. Two hint sources, both used by
  real apps: ICCCM `WM_NORMAL_HINTS` with min size == max size (the standard "don't resize me",
  which also rules out maximizing and tiling), and `_MOTIF_WM_HINTS`' `functions` field, still the
  only way a toolkit can say "this dialog has no maximize button" -- including its `MWM_FUNC_ALL`
  trap, where the listed bits are the ones to *remove*. A disallowed action's titlebar button isn't
  drawn at all (the title absorbs the width), and the operation is refused however it's invoked --
  shortcut, taskbar, `_NET_WM_STATE` message or drag -- so nothing can do what the titlebar won't.
  The same set is published as `_NET_WM_ALLOWED_ACTIONS` so taskbars and window menus grey out the
  matching entries. Everything starts allowed and is only ever taken away, so a client that
  declares nothing behaves exactly as before.
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
- Optional focus-stealing prevention (`focus_stealing_prevention=`, `none` by default so nothing
  changes until asked for) with kwin's five levels, gating the only two paths a client can take
  focus through on its own -- mapping a window and `_NET_ACTIVE_WINDOW` -- and turning a refused
  request into `_NET_WM_STATE_DEMANDS_ATTENTION` rather than a window that quietly never appears.
  See "Focus stealing prevention" below.

What's left is extras rather than missing window management: a global menu in the decoration,
gradients, per-button hover outlines in the decoration's accent color (the way Klassy does them),
fake transparency in the style of KDE 3's Crystal, and an end-to-end self-test program. Compositing
is out of scope here by design -- it belongs to [kicomp](../kicomp), a separate process kiwm never
requires.

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
| `desktop_columns` / `desktop_rows` | `0` / `1` | The shape those desktops are arranged in, numbered row-major from the top-left. Either may be `0`, meaning "as many as `num_desktops` needs" (EWMH's own semantics for `_NET_DESKTOP_LAYOUT`), so `desktop_rows=2` alone gives a 2-row grid of whatever the count is. The default -- 0 columns, 1 row -- is the single row kiwm always had. This shape is what the Meta+Tab switcher draws, and kiwm publishes it as `_NET_DESKTOP_LAYOUT`, so a pager (xispanel's reads exactly that property) shows the same grid without being configured separately. |
| `mod_key` | `meta` | The one modifier (`alt` or `meta`) kiwm claims for its mouse gestures: hold it and left-drag anywhere on a window (not just its titlebar) to move it, right-drag to resize it. Also what `ModKey` resolves to in the `key_*` shortcuts below, which is how the desktop-switch/maximize/minimize/tile defaults follow it. Replaces the old `mod_cycle=`/`mod_control=` pair, which were only ever tested together at every gesture site -- the second one bought nothing but a second modifier applications could no longer use. Both old names are still accepted (with a warning) and set `mod_key`; `ModCycle`/`ModControl` in a `key_*` spec likewise still resolve to it. The window switcher is no longer tied to this at all -- it's a plain `key_window_next=Alt+Tab` binding, so a default kiwm leaves Alt entirely to applications. |
| `border_thickness` | `0` | Left/right/bottom decoration border thickness in pixels. `0` means no border at all -- just the titlebar (the original look). |
| `border_color` | `#000000` | Fallback border color, used only when no theme `colors` file overrides it (see "Theming"). |
| `snap_threshold` | `20` | How close (pixels) the pointer must get to an output's *usable* area edge while dragging a window to snap it there -- top edge maximizes, left/right edges fill exactly half the width, Windows7/kwin-style. `0` disables snapping entirely. |
| `live_snap_resize` | `0` | Whether an edge snap (`snap_threshold` above) resizes the window *while* you drag it (`1`, kiwm's original behavior), or only draws an outline where it will land and applies that geometry when you release the button (`0`, the default). The live version means a window that jumps to half the screen and back as the pointer crosses in and out of the edge zone while you're still deciding; the outline is what xfwm shows instead. |
| `outline_width` | `16` | Thickness (pixels) of the outline kiwm draws around a window it's pointing at without moving it yet -- the snap preview above, and the switcher with `osd_live_preview_windows=0`. The band straddles the window's edge, half outside and half in, so `16` is 8px either side. Clamped to 1..64. |
| `resize_grip` | `12` | Thickness (pixels) of the invisible resize ring **around** a window: a plain click in it resizes -- from a corner where two edges meet, along one axis on an edge -- instead of going to whatever is under it, and hovering it shows the matching resize cursor. The ring sits entirely outside the frame, so it costs the application nothing (it used to be a strip *inside* the frame, where a wide grip shadowed a scrollbar sitting at the window's edge) and all four edges are the same thickness. Works with or without a visible border. Not offered on a maximized or fullscreen window (both fill their output; there is nothing to drag their edges towards), but half-tiled windows keep it, so the shared edge of a tiled pair can be dragged without a modifier. `0` disables it (resizing then needs the modifier drag). |
| `live_resize` | `1` | Whether resizing changes the window as the pointer moves (`1`, the default), or only outlines the size it's heading for and applies it when the button is released (`0`). Covers every resize the same way: the grip, a modifier-drag, or an application's own `_NET_WM_MOVERESIZE` request. |
| `osd_order` | `list` | Order the window switcher lists windows in. `list` is kiwm's own client order (stable, so a window keeps its place in the list as you switch around). `mru` is most-recently-used first, which puts the focused window at the top and makes a single Tab flip to the one before it. X has no focus history to read -- EWMH stops at `_NET_CLIENT_LIST_STACKING`, which is *stacking* order and only resembles use order while click-to-focus raises everything -- so kiwm records it itself, per focus change. |
| `magnet_threshold` | `10` | How close (pixels) a window's *edge* (not the pointer -- the frame, decoration included), while being moved or resized, must get to another window's edge, a same-output dock/panel/taskbar's edge, or the screen edge before it snaps flush against it, gap-free -- a much smaller, purely cosmetic nudge than `snap_threshold`'s tiling snap above. `0` disables it. |
| `link_resize_neighbors` | `0` | `1` makes resizing also resize whatever's touching (within 1px) the edge being dragged, oppositely, so both stay touching -- same output only. `0` (default) leaves resizing exactly as before. |
| `auto_switch_argb` | `1` | A window's frame is 32-bit (so the decoration can be translucent) only while a compositor is running; without one a 32-bit frame costs about 8 points of GPU during a resize and buys nothing. `1` (default) re-frames every window when a compositor arrives or leaves (`kicomp --toggle`, for instance), so they always have the right depth. `0` leaves each frame at the depth it was created with. |
| *(sem chave)* | — | Resize sincronizado (`_NET_WM_SYNC_REQUEST`): para clientes que anunciam o protocolo (Qt, GTK, Electron…), cada passo de um resize só é enviado depois que o cliente terminou de desenhar o anterior — conteúdo e moldura andam juntos, e o cliente nunca é redimensionado mais rápido do que pinta. Automático; um cliente que para de responder é esperado por 200 ms e depois ignorado por 2 s. |
| `focus_follows_mouse` | `0` | `1` raises+focuses a window just by moving the pointer into it ("sloppy focus"). `0` (default) requires an actual click. |
| `focus_stealing_prevention` | `none` | How much kiwm trusts a window that asks for focus **without the user having asked for it**: a window mapping itself into focus, or an application sending `_NET_ACTIVE_WINDOW` for one of its own windows. `none` (default) focuses whatever asks, which is what kiwm always did. `low` honors only a window's own explicit "don't focus me" (`_NET_WM_USER_TIME` of 0 -- a mail client starting into the tray, a session-restored window). `normal` also refuses a window whose last user interaction is older than the focused window's. `high` also refuses any application other than the one you are currently in (same ICCCM group leader, or a transient of the focused window). `extreme` never lets an application take focus on its own. See "Focus stealing prevention" below. |
| `force_unflip` | `auto` | Whether the delayed repaint rounds after a fullscreen window loses focus are sent -- the 150ms and 500ms re-exposes that survive the X server's unflip copying the last page-flipped frame over everything (see the bullet above). `auto` sends them only when no compositor owns `_NET_WM_CM_S<n>` and the server has the `Present` extension, which is the case that needs them; `always` sends them whatever else is running (kiwm's behavior since the fix); `never` sends only the immediate `ClearArea` round. The extra rounds cost two `ClearArea` sweeps over the uncovered windows, and only on a fullscreen window losing focus. |
| `osd_enabled` | `1` | `1` (default) shows a themed overlay while holding Alt+Tab/Meta+Tab, only switching on release -- see "On-screen overlays (OSD)" below. `0` reverts to switching immediately on every Tab press, no overlay. |
| `osd_live_preview_windows` | `0` | `1` applies every Alt+Tab step live (raise + focus the highlighted window) instead of only on release -- Escape then reverts to whatever was focused before the hold started. `0` (default) leaves everything untouched until release. Ignored when `osd_enabled=0`. |
| `osd_live_preview_desktops` | `0` | The same for the desktop switcher (Meta+Tab): `1` switches to the highlighted desktop on every step. Separate from the windows one because previewing a *window* raises and focuses it, which is far more disruptive than previewing a desktop. The old `osd_live_preview=` still works and sets both. |
| `osd_desktop_windows` | `1` | `1` (default) draws the windows of each desktop inside its square in the Meta+Tab desktop switcher, at their real geometry scaled down -- the same picture xispanel's pager draws with `show_windows=yes`, except kiwm already owns every window's geometry, so it costs nothing but the drawing. The squares are drawn taller when this is on, since a scaled-down window in a 64px-high square is a couple of pixels of nothing. `0` gives plain numbered squares. |
| `osd_output_follows_pointer` | `0` | `1` opens an overlay on whichever output the pointer is on (polled once when the hold starts), instead of the currently focused window's output (`0`, default; falls back to the pointer's output only when nothing is focused). Not the same as `focus_follows_mouse=` -- only decides which screen Alt+Tab/Meta+Tab themselves act on. |
| `theme` | `greenxp` | Theme folder name/path (see "Theming"). Resolved the same way kiwm looks for its own binary-relative files: tried as `../<theme>`, `./<theme>`, and plain `<theme>` (so it works both run from the source tree and installed). |
| `titlebar_layout` | `icon,title,shade,minimize,maximize,close` | Titlebar element order, left to right, comma-separated. See "Titlebar layout" below. |
| `appmenu_command` | *(empty)* | What the `appmenu` titlebar element runs when clicked, e.g. `xisserve --menu %w %x %y`. `%w` becomes the window id (decimal), `%x`/`%y` the root coordinates just under the button; `%%` is a literal percent, and anything else is passed through untouched. Run detached (double fork + `setsid()`, so no zombies and no dying with the terminal kiwm was started from). Empty (the default) means no appmenu button at all, however `titlebar_layout=` is written. kiwm deliberately has **no DBus of its own**: a `com.canonical.dbusmenu` client inside the WM would put bus round-trips in the one process that must never block, duplicating what the panel (or xisserve) already implements -- so kiwm draws the button and hands off the menu. |
| `key_*` | see below | Global keyboard shortcuts, one key per action (`key_minimize=Meta+Down`, ...). See "Keyboard shortcuts" below for the full list, the syntax, and how to unbind one. |

Two more things affect decoration/theming but aren't `kiwm.conf` keys:

- `$KIWM_DECO_BG` (environment variable): overrides just the theme background image path,
  on top of whatever `theme=` resolves to. Useful for testing a background without touching the
  theme folder itself.
- `$KIWM_HIDE_DECO_ON_MAXIMIZE` (environment variable, `0`/`no` or anything else): a quick
  override *on top of* `hide_deco_on_maximize=`, for testing without editing the config file.

### Titlebar layout

`titlebar_layout=` is a comma-separated list of any of these, in any order, any subset. It's the
layout kiwm *offers*; a given window may show fewer buttons than it lists, since an element whose
action that window doesn't permit (see "Status" above -- `WM_NORMAL_HINTS` / `_MOTIF_WM_HINTS`) is
left out entirely and the title takes the freed width:

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

- `appmenu` -- the application's own exported menu (File/Edit/View...), as a button. kiwm never
  speaks DBus for it: clicking runs `appmenu_command=` (below) with `%w`/`%x`/`%y` substituted, and
  whatever that is -- `xisserve --menu`, xispanel, a script -- draws the menu. The element is
  hidden unless a command is configured **and** the window actually exports a menu (it carries
  `_KDE_NET_WM_APPMENU_SERVICE_NAME`/`_OBJECT_PATH`, which every Qt/KF5 app sets and xispanel's
  globalmenu widget reads the same way), so it never leaves a dead button on windows with no menu.
  Since those properties are usually set a moment *after* the window maps, kiwm watches for them
  and the button appears when the menu really exists. Themes have no sprite column for it yet, so
  it falls back to a hand-drawn hamburger.

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

  The rest of the file describes the title text, rendered via Pango (per-glyph font fallback, so
  titles in scripts the default font doesn't cover -- CJK, Cyrillic, Arabic, etc. -- still show up
  instead of leaving blank gaps, plus proper `...` ellipsizing instead of a hard clip):
  ```
  font=sans-serif
  font_size=12.5
  title_center=0
  font_weight=normal
  font_style=normal
  title_shadow=none
  title_shadow_offset=1 1
  title_outline=none
  title_outline_width=1
  ```
  `font=` is any Fontconfig family name; `font_size=` is in pixels. Both default to the values
  above (kiwm's original hardcoded look) if left out or no `colors` file exists. `title_center=`
  (default `0`) centers the title within its slot instead of left-aligned with an 8px pad -- the
  title element is always the greedy one in `titlebar_layout=` (it soaks up whatever width isn't
  used by the other elements), so this is just a text alignment choice, no separate spacer element
  needed either way.

  `font_weight=` takes a name (`thin`, `ultralight`, `light`, `semilight`, `book`, `normal`,
  `medium`, `semibold`, `bold`, `ultrabold`, `heavy`/`black`) or a raw Pango weight number
  (`100`-`1000`); `font_style=` takes `normal`, `italic` or `oblique`. Both are applied to the
  shared font description **only while the title is drawn** and put back afterwards, so a bold
  title doesn't also bold the switcher and window-menu text.

  One more group tints the titlebar *buttons* on hover, the way Klassy colors its:
  ```
  button_tinting=over
  button_tint_scope=button
  close_button_tint=#dd2222aa
  maximize_button_tint=none
  minimize_button_tint=none
  shade_button_tint=none
  keep_above_button_tint=none
  keep_all_desktops_button_tint=none
  ```
  One key per button, named after the same word `titlebar_layout=` uses to place it, each off
  until given a color (`none` switches one back off). The tint applies **only while the pointer is
  on that button** -- a toggle button being *on* is not a pointer state and keeps the theme's
  normal active look. `button_tinting=` decides what the color does: `over` (default) composites it
  over the button as drawn, so a translucent tint still lets the sprite's own shading read through
  (that's what the alpha is for); `replace` paints the button's own shape flat in that color
  instead, for a sheet whose artwork fights the tint; `none` ignores every tint without having to
  delete them. Either way the tint is masked by the button sprite's alpha, so it follows the
  button's actual shape -- a round button stays round, and the cell's transparent corners stay
  transparent -- rather than washing a rectangle of color across the slot.

  `button_tint_scope=` says how far the tint reaches. `button` (default) is the button itself;
  `decoration` puts the hovered button's color over the whole titlebar **and its borders**
  instead, so pointing at Close turns the entire frame red for as long as the pointer is there,
  and moving to Minimize turns it that button's color; `both` does the two at once. With
  `decoration`/`both` the wash is painted before the title and the buttons, so those are still
  drawn on top and stay legible -- the frame recolors rather than blanks -- and `replace` there
  means the tint stands in for the theme background and focus tint entirely, rather than being
  laid over them.

  `title_shadow=` and `title_outline=` are off until given a color (`#rrggbb`/`#rrggbbaa`, or
  `none` to switch one back off), and the color is the switch -- there's no separate enable key.
  The shadow is the same text drawn again underneath, offset by `title_shadow_offset=` (`dx dy`,
  or a single number for both axes; negatives cast it up and/or to the left). Deliberately a hard
  drop shadow, not a blurred one: a blur would mean a second surface per title draw, and at
  titlebar sizes this is what reads as a shadow anyway. The outline strokes the glyph outlines --
  Pango hands the whole glyph run to cairo as a path, so it's one stroke, not a per-glyph redraw
  -- at `title_outline_width=` pixels (default `1`, capped at `8`), straddling each letter's edge
  so a 1px outline doesn't swallow thin strokes. Both effects keep their alpha, unlike
  `fg_active`/`fg_inactive` (whose alpha is about the titlebar's translucency, so the name stays
  readable); drawing order is shadow, then outline, then the fill on top.

### Mouse and keyboard reference

Every keyboard shortcut below is rebindable -- see "Keyboard shortcuts". The mouse gestures aren't
separately bindable; they follow `mod_key=` directly. With the default (`mod_key=meta`):

- **Click** a window (titlebar or content): focus + raise.
- **Click-drag** a titlebar: move. Drag to a screen edge to snap (see `snap_threshold=` above) --
  which by default outlines where the window will land and only resizes it on release, see
  `live_snap_resize=`.
- **Double-click** a titlebar (not on a button): maximize/restore.
- **Scroll** a titlebar: shade/unshade.
- **Right-click** the decoration (titlebar or border), or **click the window icon**: the window
  context menu -- see "Window context menu" below.
- Titlebar buttons: whatever `titlebar_layout=` configures, left to right. They act on **release**,
  and only when the release lands on the same button the press did -- dragging off a button before
  letting go cancels it, and the button is drawn held down (`btns.png`'s third row) in between.
- **Maximize button**: left click maximizes both directions; **right click** maximizes horizontally
  only, **middle click** vertically only, the way kwin's does. From either single-axis state a left
  click takes the window the rest of the way, and the click after that restores it to the geometry
  it had before any of it.
- **Click-drag a window's edge or corner**: resize, no modifier needed, decoration or not -- see
  `resize_grip=` above. A corner resizes both axes; an edge only its own. The grip is a ring of
  invisible `InputOnly` windows *outside* the frame, one per edge and corner, each carrying its own
  resize cursor -- so it takes none of the application's pixels and all four edges work the same
  way, the top one included. Not offered on maximized or fullscreen windows, which fill their
  output by definition.
- **Dragging a fullscreen window** doesn't move it around its screen -- it carries it to another
  *output*, still fullscreen, which is otherwise impossible without leaving fullscreen first (kwin
  behaves the same). Nothing follows the pointer: the outline shows which output would take it, and
  releasing re-applies fullscreen there. Resizing one is refused outright.
- **Meta+drag** (left button): move a window from anywhere on it, not just its titlebar. Dragging
  a maximized or tiled window only restores it once the pointer has moved a titlebar's height --
  see "Status" above.
- **Drag empty space inside a window**: works for apps that ask kiwm to do it via
  `_NET_WM_MOVERESIZE` -- Qt Breeze/Oxygen toolbars, GTK headerbars, Steam's own chrome.
- **Meta+right-drag**: resize, from whichever corner of the window is nearest wherever you
  clicked -- the opposite corner stays fixed.
- **Alt+Tab** / **Alt+Shift+Tab**: hold Alt, tap Tab/Shift+Tab to step forward/backward through
  mapped windows on the current output's current desktop (plus any sticky ones), each highlighted
  one outlined where it sits (see "The outline" below) -- releasing Alt commits whichever is
  highlighted (see "On-screen overlays (OSD)" below; `osd_enabled=0` switches
  immediately on every tap instead, with no overlay).
- **Meta+Tab** / **Meta+Shift+Tab**: same idea, for the focused output's current desktop. Switching
  desktops **while dragging a window** carries that window along to the new desktop, the way kwin
  and compiz do -- keep the mouse button held, tap Meta+Tab, and the window travels with the
  pointer instead of being left behind. Any way of switching does it, since they're the same
  gesture from the user's side: the cycle shortcut, a direct `key_desktop_N` jump, or a pager click.
- **Escape**: while either overlay is open, cancel without switching.
- **Meta+Up**: maximize/restore the focused window.
- **Meta+Down**: minimize the focused window.
- **Meta+Left** / **Meta+Right**: tile the focused window to the left/right half of its output's
  usable area -- the same geometry a drag to that screen edge produces, without the drag. Pressing
  it again for the side the window is *already* tiled to restores it, so one key both tiles and
  untiles.
- **`key_desktop_1`..`key_desktop_8`**: jump the focused output straight to that desktop
  (immediate, no overlay -- a direct-select shortcut, not a cycle). Unbound by default.
- **`key_move_to_desktop_*`**: send the focused window to another desktop without following it.

### Focus stealing prevention

`focus_stealing_prevention=` (default `none`) decides what happens when a window wants the keyboard
and *you* didn't ask for it. Two things a client can do reach this: mapping a window (a new window
normally takes focus) and sending `_NET_ACTIVE_WINDOW` for one of its own windows. Everything you
do yourself -- clicking a window, the switcher, the window menu, the `key_*` shortcuts, or a
taskbar/pager activating a window (EWMH marks those messages as source `2`) -- is never subject to
any of it, at any level.

The levels follow kwin's, since that's the vocabulary anyone configuring this already has:

| level | refuses |
|---|---|
| `none` | nothing -- kiwm's behavior so far, and still the default |
| `low` | a window whose `_NET_WM_USER_TIME` is `0`, EWMH's explicit "do not focus me on map" |
| `normal` | ...also a window whose last user interaction is older than the focused window's |
| `high` | ...also any application other than the one you are in (same ICCCM group leader, or a transient of the focused window) |
| `extreme` | ...everything: no application ever takes focus on its own |

A window whose request is refused is **not** hidden or held back: it maps, it is stacked normally,
it just doesn't take the keyboard, and kiwm sets `_NET_WM_STATE_DEMANDS_ATTENTION` on it so a
taskbar highlights the entry (xispanel's tasklist reads that state). Focusing it, however you do
that, clears the flag. A client can also set and clear the state itself -- a chat window with a new
message -- through the ordinary `_NET_WM_STATE` message, and kiwm now advertises the state in
`_NET_SUPPORTED` either way.

`_NET_WM_USER_TIME` is read straight off the window at decision time (following
`_NET_WM_USER_TIME_WINDOW` when the client points it elsewhere, as Qt and GTK do) rather than
cached: focus decisions are rare, and reading on the spot avoids tracking `PropertyNotify` on a
window kiwm otherwise never touches. A window carrying no user time at all is given the benefit of
the doubt at `normal` (plenty of small apps never set it) and refused at `high`/`extreme`.

### Keyboard shortcuts

All of kiwm's global keyboard shortcuts live in `kiwm.conf` as `key_*` keys, and a freshly
generated config lists every one this build has, with its default:

```
key_window_next=Alt+Tab
key_window_prev=Alt+Shift+Tab
key_desktop_next=ModKey+Tab
key_desktop_prev=ModKey+Shift+Tab
key_desktop_next_horizontal=
key_desktop_prev_horizontal=
key_desktop_next_vertical=
key_desktop_prev_vertical=
key_maximize=ModKey+Up
key_maximize_horizontal=
key_maximize_vertical=
key_minimize=ModKey+Down
key_tile_left=ModKey+Left
key_tile_right=ModKey+Right
key_fullscreen=
key_shade=
key_keep_above=
key_keep_below=
key_sticky=
key_close=Alt+F4
key_window_menu=
key_desktop_1=
key_desktop_2=                 # ...through key_desktop_8, unbound by default
key_move_to_desktop_next=
key_move_to_desktop_prev=
key_move_to_desktop_1=
key_move_to_desktop_2=         # ...through key_move_to_desktop_8, unbound by default
```

- Syntax is `Mod+Mod+Key`, case-insensitive: `Meta+Down`, `alt+shift+Tab`, `Ctrl+Alt+F1`.
- Modifiers: `Alt`, `Meta` (= `Super`/`Win`), `Ctrl`, `Shift`, plus `ModKey`, which resolves to
  whatever `mod_key=` is set to. The window-control defaults use the symbolic form on purpose, so
  setting `mod_key=alt` moves them all along with it. `ModCycle`/`ModControl` are the pre-merge
  spellings and still parse, both meaning `ModKey`, so an older config's `key_*` lines don't turn
  into "unknown modifier" warnings all at once.
- The window switcher's defaults name **Alt literally**, not `ModKey`: switching windows is a plain
  shortcut like any other, and pulling the mouse-gesture modifier onto Tab is exactly what used to
  cost you a second modifier. With the shipped defaults, `Alt+Tab`/`Alt+Shift+Tab` and `Alt+F4` are
  all kiwm takes from Alt, and none of them involve `mod_key`.
- **Two bindings resolving to the same modifiers+key** would silently shadow each other (dispatch
  runs the first one in the table), so kiwm warns and leaves the later one unbound. Worth knowing
  when migrating an old config: `ModCycle+Tab` and `ModControl+Tab` now mean the same combination.
- `key_move_to_desktop_*` sends the focused window to another desktop of its own output *without*
  following it there -- pair one with a `key_desktop_*` binding to do both. The `next`/`prev` pair
  steps from wherever the window currently is, not from the output's current desktop.
- Keys are named like their X keysyms (`Tab`, `Up`, `Down`, `Left`, `Right`, `Escape`, `Return`,
  `space`, `Home`, `End`, `PageUp`, `PageDown`, `Delete`, `F1`-`F12`), a single printable character
  (`a`, `7`, `/`), or a raw `0x<hex>` keysym for anything else.
- An **empty value** leaves that action unbound (and ungrabbed) -- that's what the actions with no
  default above are. A line that's present but empty wins over the built-in default, so a shortcut
  can be turned off, not just moved.
- Every shortcut is grabbed with and without NumLock/CapsLock, so none of them silently stop
  working with either lock key on.
- The four `key_desktop_*_horizontal`/`_vertical` shortcuts move one column or one row at a time
  through the `desktop_columns` x `desktop_rows` grid, instead of walking the desktops in index
  order the way `key_desktop_next`/`prev` do. All six open the same overlay and can be mixed
  within one hold, and all six are independent: binding only `key_desktop_next_vertical` and
  leaving its `prev` empty gives exactly one direction, which is a perfectly reasonable thing to
  ask for. They're unbound by default, so a config that never mentions them keeps the plain
  Meta+Tab / Meta+Shift+Tab pair kiwm always had.
- A switcher hold is ended by releasing **the modifier its own binding names** (Shift excluded --
  a `prev` binding is the same hold as its `next` one), not by a fixed `mod_key`.
  So `key_desktop_next_vertical=Ctrl+Alt+Down` holds on Ctrl+Alt and commits when those come up.
- **Escape** isn't in the table: it's the fixed cancel key for whatever hold is in progress (the
  overlays below), never grabbed and not rebindable.

### On-screen overlays (OSD)

With `osd_enabled=1` (the default), holding a switcher shortcut's modifier (Alt for windows, Meta
for desktops by default) and tapping Tab opens a themed overlay -- same background/border colors and corner radius as the
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
  output's real resolution, like xispanel's pager widget), the pending selection highlighted. The
  grid is laid out in `desktop_columns` x `desktop_rows` -- the same shape kiwm publishes as
  `_NET_DESKTOP_LAYOUT`, so the overlay and any pager on screen agree -- with the cells of a short
  last row simply left empty.
  With `osd_desktop_windows=1` (the default) each square also carries the windows that live on
  that desktop, drawn as little rectangles at their real geometry scaled into the square -- the
  focused one filled a shade brighter. kiwm decided all that geometry itself, so nothing has to be
  asked for over the wire the way an outside pager would have to.
- The list is in kiwm's own client order by default; `osd_order=mru` makes it most-recently-used
  first instead, so one Tab flips between the last two windows. Either way the hold starts on the
  focused window, so the first Tab step lands on the next entry.
- By default (`osd_live_preview_windows=0`), nothing actually changes until the modifier is released --
  browse freely, decide, then let go. With `osd_live_preview_windows=1`, every step already applies live
  (raises+focuses the highlighted window, or switches to the highlighted desktop) as you move
  through it, same as most desktops' Alt+Tab -- **Escape** then reverts back to whatever was
  actually focused/current *before* the hold started, not just "cancels" a no-op.
- **Any mouse click** ends the hold too, exactly as if the modifier had been let go (the current
  selection is committed), and the click itself still does whatever it was going to do -- it's
  replayed to whoever would normally have received it.
- A window that closes while the Alt+Tab list is open (e.g. a crash) is quietly dropped from the
  list in place, selection re-clamped -- it's never focusable, and if it was the only entry left
  the overlay just closes. If it was also the window `osd_live_preview_windows`'s Escape-revert was going
  to restore, that revert is dropped too (there's nothing left to revert to).

Internally, kiwm actively grabs the keyboard (`xcb_grab_keyboard()`) for as long as an overlay is
open -- the only reliable way to see the modifier key's own release regardless of which client (if
any) has input focus; a plain `xcb_grab_key()` binding (what every other kiwm shortcut uses) can't
by itself. Committing is decided by *live keyboard state*, not by matching the released key against
a specific hardcoded keycode: every KeyRelease while an overlay is open re-queries whether
the driving modifier's bit is still set at all (`xcb_query_pointer()`'s modifier mask) and only
commits once it's actually gone. Matching one fixed keycode (e.g. just `Alt_L`) instead would miss a
layout where the modifier lives on a different/second physical key, or misfire on a modifier key's
own X autorepeat -- either way leaving the keyboard grab stuck engaged (every keystroke system-wide
silently swallowed by an OSD nobody can see) until something else forced it shut, which is exactly
the "100% CPU, no window will open" wedge that shipped in this feature's first cut.

The keyboard grab is not a *guarantee* that the release will ever arrive, though: a client that
grabs the input devices for itself while an overlay is up -- VirtualBox capturing input for its
guest is the real-world case, and Alt+Tab with `osd_live_preview_windows=1` walks right into it, since the
preview hands the VM focus mid-hold -- swallows it, and then no further event of any kind arrives
to notice it with. The overlay would just sit there forever, keyboard still grabbed. So there are
two backstops. While an overlay is open the event loop also wakes up every 100ms and re-checks the
live modifier state itself (`osd_poll_release()`), which closes it within a tenth of a second even
if kiwm never receives another input event at all; and the pointer is grabbed alongside the
keyboard so a click can end the hold as well (in `GrabModeSync`, since only an event the freeze is
still holding can be replayed to its real target afterwards -- and an active sync grab freezes the
pointer from the moment it's taken, so kiwm has to issue an explicit `AllowEvents`/`SyncPointer`
right after grabbing or the click never gets reported to it either). Neither grab is treated as
required: both are allowed to fail (another client may already hold one), the overlay opens
regardless, and the failure is logged.

The window list
is built behind a small `TabBoxOps` vtable (`osd.h`/`osd.c`) -- kwin calls the same idea a "tabbox"
-- so a different presentation (a thumbnail grid, cover-flow, ...) can be swapped in later by
writing a new `TabBoxOps` and pointing one variable at it, with no changes to the hold/release
mechanics or eligibility rules. Only one implementation exists today: `simple_list_tabbox_ops`, the
plain list above. The desktop grid isn't behind such a vtable -- it's a single fixed presentation,
not asked to be swappable.

### The outline

Two things draw a hollow rectangle around a window instead of touching the window itself -- the
classic wireframe preview, in the focused decoration's own color (flat, no theme image), a band
straddling the window's edges -- `outline_width=` pixels thick, half outside and half in (8px
either side by default):

- **Alt+Tab with `osd_live_preview_windows=0`** (the default): nothing is raised or focused until the
  modifier is released, so the switcher list alone doesn't say *where* the highlighted window
  actually is. The outline does, the way xfwm's does.
- **Resizing with `live_resize=0`**: the window stays as it is for the whole drag and the outline
  shows the size it's heading for, applied once on release -- along with an outline for every
  window a linked resize (`link_resize_neighbors=`) is dragging along with it, since those are part
  of what releasing the button will do.
- **Dragging a window to a screen edge with `live_snap_resize=0`** (the default): the window keeps
  following the pointer, and the outline shows the size and position it will take when the button
  is released.

It's one override-redirect window, XCB SHAPE-clipped down to just the band so the middle stays a
real hole with the window underneath showing through, and with an empty *input* shape so it can
never intercept a click -- including during the drag it's previewing. It is painted by *being* its
color rather than by drawing into it (the window's background pixel is the decoration color), so
the server fills whatever a resize exposes as part of the same operation that resizes it -- drawing
the color in afterwards showed as a visible flash of the old contents at the new size on every step
of a drag. It has its own stacking layer
(`outline` in the list above) directly below `osd`: above every client, including an active
fullscreen one, but under the switcher overlay that's usually driving it.

### Window context menu

Right-clicking a window's decoration (titlebar or border, without a modifier -- with one that's the
resize gesture) or left-clicking its icon opens the window menu: minimize, maximize/restore, shade/
unshade, move to desktop (a submenu of the output's desktops, the window's own checked), all
desktops, keep above, close. Entries the window doesn't allow are dimmed and unselectable, from the
same `_MOTIF_WM_HINTS`/`WM_NORMAL_HINTS`-derived permissions the titlebar buttons already follow
(see "Status" above), so a dialog that can't be maximized doesn't offer it here either.

It's drawn by kiwm itself in the decoration's own colors and corner radius, XCB SHAPE-clipped like
everything else here, with the row styling (hover wash, separator, dimming, submenu arrow) matching
xispanel's menus so the two programs' menus read as the same widget. Navigation works by mouse or
keyboard: arrows move, Right/Left open and close a submenu, Enter/Space pick, Escape backs out one
level (and closes at the top). A click outside dismisses it.

**Adding an action** is deliberately cheap: one row in `entries[]` at the top of `menu.c` -- its
label, an optional second label for when it's "on" (Maximize/Restore), an optional predicate for
whether the window allows it, an optional predicate for its current state (drawn as a check mark) --
plus one case in that file's `run_action()`. Nothing else needs to know it exists. Submenus are a
stack of popup windows, one per level, so a second level is a new submenu kind and its builder, not
a rework.

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
- `shape.c` -- forwarding a client's own non-rectangular SHAPE onto its frame (bounding + input),
  and deciding between that and decoration.c's rounded corners.
- `keybind.c` -- configurable global keyboard shortcuts: one table holding every action, its
  `kiwm.conf` key, its default binding and its generated documentation, plus the spec parser, the
  root-window grabs and the dispatch (see "Keyboard shortcuts" above).
- `selection.c` -- `--replace` (ICCCM manager-selection handoff).
- `osd.c` -- Alt+Tab/Meta+Tab on-screen overlays (see "On-screen overlays (OSD)" above).
- `menu.c` -- the window context menu, one table of actions plus the popup/submenu machinery (see
  "Window context menu" above).
- `outline.c` -- the wireframe rectangle drawn around a window kiwm is pointing at without moving
  it yet (see "The outline" above).

`kiwm-gpt.c` is an earlier, single-file GPT-authored attempt (single global workspace, no RandR,
no theming) kept only as historical reference -- not built by the Makefile, not maintained.
