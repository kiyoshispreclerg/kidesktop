# xisserve launch contract

This is the contract `xispanel`'s `xisserve` widget
(`../xispanel/widgets/xisserve.c`) already implements and relies on.
Read this before writing any real UI code here -- it's the one thing a
future xisserve session must not silently drift away from, since the
widget side is already shipped and won't be revisited just because
xisserve's own argument parsing changes shape.

## How xispanel invokes xisserve

On every click of the `xisserve` widget -- and of any other widget that
opens xisserve anchored to itself, see `--calendar` below -- xispanel runs
(via `sh -c`, detached, `cmd=` config key overrides the binary name/path,
default `xisserve` resolved through `$PATH`):

```
xisserve --anchor-x=<px> --anchor-y=<px> --anchor-w=<px> --anchor-h=<px> \
         --edge=top|bottom|left|right \
         --output-x=<px> --output-y=<px> --output-w=<px> --output-h=<px> \
         --bg=#RRGGBBAA --fg=#RRGGBBAA \
         --font=<family name> --font-size=<px>
```

All coordinates are root-window pixels (not relative to the panel or to
any output). All flags are always present, in this order, every time --
no flag is ever omitted, so a simple positional or `getopt_long` parser
is enough; nothing needs `--flag=value` split-on-`=` beyond what
`getopt_long` already gives you.

- `--anchor-x/-y/-w/-h`: the exact on-screen rectangle of the panel
  button that was clicked. Position xisserve's window glued to the
  panel's outer edge, aligned to this rectangle's leading edge -- the
  same convention `xispanel`'s own tooltips and context menus already
  use (see `xispanel/menu.c`'s `panel_menu_open_tree_lazy()`): open below
  the anchor when `--edge=top`, above it when `--edge=bottom`, to the
  right when `--edge=left`, to the left when `--edge=right`.
- `--output-x/-y/-w/-h`: the geometry of the RandR output the panel
  lives on. Clamp xisserve's window to stay fully inside this rectangle
  (same "never render off the actual screen" rule the menu/tooltip code
  applies) -- relevant on multi-monitor setups where the anchor sits
  near an output's edge.
- `--bg`/`--fg`: the owning panel's own theme colors, `#RRGGBBAA` hex
  (alpha included, always 9 characters). Use these for xisserve's
  window background/text instead of (or blended with) GTK2's own theme,
  so the popup doesn't look like a foreign app dropped on top of the
  panel. Exactly matching plasmashell/kickoff's chrome isn't the goal --
  just not clashing.
- `--font`/`--font-size`: the UI font from xispanel's own config
  (`THEME`'s `font=`, or Fontconfig's default when unset) and the panel's
  resolved text size in pixels. `--font` is a bare family name
  (e.g. `Comic Relief`), never pre-quoted -- your argv parser gets it
  as one whole string already, no shell-unescaping needed on your end.

A mode flag may follow the ones above, selecting which page/tab xisserve
opens instead of its default launcher view. An xisserve that doesn't know
a flag must ignore it and open normally rather than fail to start -- the
widget side ships before the page does, every time. (This is enforced in
`parse_argv()` via `opterr = 0` plus an ignore-by-default switch, and is
live right now: `--notifications` below is already being sent by a
shipped widget with no page implemented for it.)

- `--calendar`: passed by xispanel's `clock` widget when its clock is
  clicked, anchored to the clock's own rectangle. Opens a navigable
  GtkCalendar (month view, current month, today highlighted, prev/next
  month and direct year entry built into the widget itself) instead of
  the launcher's search/list view. Implemented as its own window size
  (no forced minimum -- the popup shrinks to the calendar's own natural
  size) and its own reposition pass, so it still clamps fully inside
  `--output-*` on outputs much smaller than the launcher's own fixed
  size. A second invocation toggles/repositions/retheme exactly like the
  launcher view (see "Singleton / toggle behavior" below) -- switching
  between calendar and launcher mode on an already-open xisserve still
  just closes it on that click, same as any other toggle; the new mode
  takes effect on the *next* open.

- `--audio`: passed by xispanel's `volume` widget on left click,
  anchored to the volume icon. Shows the streams currently playing or
  recording (each with its own level and mute -- the per-application
  mixer view plasmashell's volume applet has) plus the output and input
  devices, each with level, mute, which one is default, and whether it's
  active. xispanel's own volume widget deliberately covers none of that:
  it only touches `@DEFAULT_SINK@` (scroll = level, middle click =
  mute), and its `cmd_edit=` still shells out to a full mixer on right
  click.

  State comes from `pactl`, not a linked libpulse/libpipewire -- so one
  code path covers PulseAudio and PipeWire, with no build-time
  dependency and nothing to ifdef (see `pages/pulse.h`). With no sound
  server answering, the page renders an explanatory placeholder rather
  than failing: xisserve still starts and every other view keeps
  working. ALSA-only systems land there too, and are told why -- bare
  ALSA has no per-application streams at all, that concept being
  precisely what a sound server introduces.
- `--notifications`: passed by xispanel's `notif` widget on left click,
  anchored to the bell icon. Should show the notification history --
  xispanel's notifd.c ring buffer is in xispanel's process, not
  xisserve's, so this page needs to read the history from somewhere:
  either xispanel grows a control-socket query for it, or xisserve reads
  the same history xisnotif is planned to keep (see
  `../XISDESKTOP_PLAN.md`). Deciding that is part of implementing this
  flag. **Not implemented yet** -- meanwhile the widget's right click
  still opens the same history inline as a panel menu, which needs no
  second process at all.

## Singleton / toggle behavior (xisserve's own responsibility)

xispanel does **not** track whether xisserve is already running, hold a
PID, or speak to any control socket -- it fires the exact command above,
unconditionally, on every single click, the same way clicking a taskbar
icon for a possibly-already-open app works. This means:

- xisserve **must** be a singleton, using the same `flock` pattern
  `xisback`/`xisguard` already use
  (`$XDG_RUNTIME_DIR/xisserve.lock`) -- a second invocation while one is
  already running must not open a second window.
- A second invocation should be treated as "reposition + retheme using
  the new argv, then toggle visibility" -- i.e. if xisserve is currently
  hidden, show it (repositioned/rethemed per the new anchor); if it's
  currently shown, hide it. This gives the widget click a natural
  open/close toggle for free, without xispanel needing to know xisserve's
  visibility state at all.
- The simplest way to hand the new argv to an already-running instance:
  have the second invocation connect to a small control socket the first
  instance opened (line-JSON, same shape as `xisguard-ctl`/
  `xisback`'s control sockets elsewhere in this repo) and send the parsed
  flags across, then exit immediately itself. Not mandated by this
  contract -- any mechanism that achieves the same observable behavior
  (second click while open = closes; second click while closed = opens
  at the new position) is fine.

## Out of scope for this contract

- No environment variables are used for any of this -- argv only (an
  earlier design considered envvars for position/theme, but argv keeps
  a `ps`/`ps -ef` listing self-documenting and matches the precedent
  already set for `launcher`'s `cmd=`/`folder`'s open-terminal actions
  in xispanel, which all shell out with the relevant values inline
  rather than through the environment).
- Search backend, `.desktop` parsing, icon theme resolution, the HUD/
  in-app-action-search idea -- see `README.md`'s "Planned scope", still
  entirely unbuilt as of this writing.
- Matching KDE Plasma's actual color *scheme* (not just the panel's own
  bg/fg) in GTK2 -- see `README.md`'s "Open question" section.
