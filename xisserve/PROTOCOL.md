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
- `--energy`: AC/battery status, other UPower devices' batteries
  (wireless mouse/keyboard, etc.), a screen brightness slider, and night
  light (checkbox + color-temperature slider). State comes from `upower`,
  `brightnessctl` and `xsct` (https://github.com/faf0/sct), each shelled
  out to rather than linked -- see `pages/power.h`. Each section only
  shows when its own tool is available, so a system missing one of the
  three still gets a useful page for the other two.

  Night light's checkbox switches between "kiconfd applies it on the
  schedule set in kiconf's Energia tab" and manual: unchecked, dragging
  the slider calls `xsct` directly and immediately. Both this page and
  kiconf write the same `kiconfd-nightlight.conf`; kiconfd is the only
  one that reads the schedule (start/end) out of it -- see kiconfd.c's
  own doc comment.
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

## `--menu`: an application menu popup for a window manager

`--menu` is the one mode that is **not** part of the xispanel contract
above and **not** subject to the singleton behavior below. It exists for
a window manager that wants an application-menu button in its decoration
without becoming a DBus client itself -- kiwm's `appmenu` titlebar
element does exactly that, running `kiwm.conf`'s `appmenu_command=` on
each click:

```
xisserve --menu <window> <x> <y>
xisserve --menu --window=<id> --menu-x=<px> --menu-y=<px>
```

Both spellings are accepted (and can be mixed; a flag wins over the
positional in its slot). `<window>` is an X window id, decimal or
`0x`-prefixed -- `0`, or no window at all, means whatever
`_NET_ACTIVE_WINDOW` points at, since a decoration click doesn't
necessarily make that window active first. `<x>`/`<y>` are root
coordinates for the menu's top-left corner; GTK still keeps the menu on
screen if that would run it off an edge.

xisserve reads `_KDE_NET_WM_APPMENU_SERVICE_NAME`/`_OBJECT_PATH` off the
window (the same pair the `globalmenu` search plugin reads off the active
one), fetches the whole tree with one DBusMenu `GetLayout(0, -1)`, pops
it as a real cascading GTK menu, and exits when the menu is dismissed or
an item is chosen -- an activation goes back to the application as a
DBusMenu `Event`, exactly as choosing it in the app's own menu bar would.
The process lives for as long as the menu is on screen and no longer.

It deliberately takes **no** singleton lock and opens no control socket:
a menu popup is per-click and transient, and relaying it into a running
launcher would leave that instance's window fighting for the position and
the input grab. Exit status is non-zero (with a message on stderr) when
the window exports no menu or the menu comes back empty -- kiwm hides the
button for such windows anyway, so this is the belt to that suspenders.

## `--applications`: a cascading menu of every installed app

`--applications` is, like `--menu`, **not** part of the xispanel contract
above and **not** subject to the singleton behavior below. It exists to be
bound as an xisback click or scroll action (see xisback's `PROTOCOL.md`,
"Click and scroll actions"), so right-clicking bare desktop space can pop
an "Applications" list the way Plasma/kickoff-style shells do:

```
xisserve --applications [<x> <y>]
xisserve --applications --apps-x=<px> --apps-y=<px>
```

Both spellings are accepted (and can be mixed; a flag wins over the
positional in its slot), same as `--menu`'s `<window> <x> <y>`. When
neither a flag nor a positional argument gives `<x>`/`<y>`, they default
to the `XISBACK_CLICK_X`/`XISBACK_CLICK_Y` environment variables xisback's
`run_action()` sets on the child it execs -- so `xisback --on-right-click
'xisserve --applications'` needs no argument wiring at all, the popup just
appears where the click landed.

Every `.desktop` file xisserve would otherwise list in its own launcher
window is scanned fresh (independent of any running launcher instance),
bucketed into the same freedesktop categories the launcher's category
pane uses, and shown as a real cascading GTK menu: top level one item per
category in use, each opening a submenu of that category's apps
(alphabetical, with icons). Choosing an app launches it exactly like
clicking it in the launcher would; dismissing the menu (Escape, a click
outside) exits with no side effect. Like `--menu`, it takes no singleton
lock and opens no control socket -- one process per popup, living only as
long as the menu is on screen.

## `--question`: a Zenity-style question popup

`--question` is, like `--menu`, **not** part of the xispanel contract
above and **not** subject to the singleton behavior below -- any number
of `--question` instances can be up at once, each its own process:

```
xisserve --question --text=<pergunta> --button=<rotulo>:<valor> [--button=<rotulo>:<valor> ...]
```

`--text` is the question shown in the popup; each `--button` (1-8 of
them, repeatable) adds one plain GTK2 button labeled `<rotulo>` that
closes the dialog and answers with the int `<valor>` -- split on the
*last* `:` in the argument, so a label is free to contain its own colons.
Example:

```
xisserve --question --text="Fechar sem salvar?" --button="Cancelar:0" --button="Fechar:1"
```

The chosen value is printed to stdout and also used as the process exit
code (low byte only -- an answer outside 0-255 still prints correctly on
stdout, but a caller reading only the exit status needs to keep its
values in that range). Dismissing the dialog with no button clicked (the
WM's close button, or Escape) exits 1 with no stdout; a usage error
(missing `--text` or no `--button`) exits 2.

It takes no singleton lock and opens no control socket, the same
reasoning as `--menu`: a question is a one-shot, per-call thing, and
relaying it into an already-running instance would mean fighting that
instance's window for the position and the input grab -- worse, here it
would also make two unrelated callers' questions collide into one popup.
The dialog carries no `--bg`/`--fg`/`--font` theming either; it's a plain
GTK2 window matching whatever GTK2 theme is already active on the
session.

## `--keyboard`: an on-screen QWERTY keyboard

```
xisserve --keyboard
xisserve --keyboard --output-x=<px> --output-y=<px> --output-w=<px> --output-h=<px>
```

Also outside the xispanel contract and the launcher's own singleton
below, but not a one-shot popup either: it opens a mouse-driven on-screen
keyboard docked to the bottom of the given output rectangle and stays up
until closed. `--output-*` is normally left out entirely -- unlike the
launcher's other pages, nothing anchors `--keyboard` to a specific panel
button, so on a multi-monitor session it instead looks up which RandR
monitor the currently focused window (falling back to the pointer) is
actually on and docks there, rather than trusting parse_argv()'s
"0,0,1920,1080" fallback default and risking landing on a screen the
user isn't even looking at -- this detection runs unconditionally and
overrides whatever `--output-*` gave (or defaulted to); an explicit
`--output-*` only actually takes effect if the lookup itself fails (no
RandR, nothing focused and no pointer either, ...).
Every key is a real button; clicking one synthesizes the actual keycode
via XTest, so whatever window currently has input focus receives it --
this window itself never takes focus (`WM_HINTS` input=False) and is
never a click-to-focus target, the same contract xispanel's own dock
windows use. Shift/Ctrl/Alt/Super are one-shot latches (click one, click
a key, the chord fires and every latch clears); Caps Lock is a real
toggle of the server's own lock state, reflected in a lamp alongside Num
Lock/Scroll Lock.

The board itself is swappable, Android-keyboard-style: a "Layout" key
cycles through every registered layout (QWERTY, QWERTY ABNT2 with a Ç
key, and an Emoji grid ship today), each one just a data table in
keyboard.c -- adding one (AZERTY, a 12-key kana board, a math-symbol
board, ...) needs no change anywhere else in the file. A layout entry
that needs a character the loaded X keymap has no key for at all (ABNT2's
Ç, every emoji) gets it by briefly remapping one otherwise-unused keycode
to that exact Unicode codepoint and sending that -- the same trick
`xdotool type` uses, since core X11/XTest has no "just send this
codepoint" call.

It keeps a *small* singleton of its own, entirely separate from the
launcher's (`$XDG_RUNTIME_DIR/xisserve-keyboard.lock`, not
`xisserve.lock`): the first invocation opens the keyboard and blocks
until it's closed (its own close button, or `SIGTERM`); a second
invocation while one is already up reads that instance's PID from the
lock file and signals it instead of opening a second keyboard -- run the
same command again to toggle it off, which is what a hotkey binding
wants. See keyboard.c.

## `--session`: a "what do you want to do" picker

```
xisserve --session
```

Also outside the xispanel contract and the launcher's own singleton, and
a one-shot popup like `--menu`/`--question` rather than something that
stays up (like `--keyboard`). It shows a small always-on-top window,
centered on whichever RandR monitor the focused window (falling back to
the pointer) is actually on -- the same lookup `--keyboard` uses to dock
itself, see that section above -- with one button per power action the
launcher's own footer already offers: Desligar, Reiniciar, Suspender,
Sair, Trocar usuario (only shown if `dm-tool` is installed), Bloquear
tela. Clicking one shows the exact same Yes/No confirmation the footer
button does and, on Yes, runs the exact same command -- both draw from
one shared table (see xisserve.h's `xisserve_power_action_*` functions)
so there is exactly one place each command lives. Escape, its own "X" or
"Cancelar" button, or the WM's close button dismiss it with no action
taken and no side effect; picking "No" on a confirmation leaves the
picker open so another action can still be chosen.

Meant to be what a power/session hotkey calls instead of the raw command
directly -- xiskeys' shipped defaults point `Ctrl+Alt+Delete`,
`Ctrl+Alt+End`, `XF86Sleep`, `Meta+Shift+L` and `Meta+Shift+U` at
`xisserve --session` rather than each running its own
`systemctl reboot`/`poweroff`/`suspend`/logout/switch-user command
straight away, so a stray tap always asks first through one shared
picker.

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
