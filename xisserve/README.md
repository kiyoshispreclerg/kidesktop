# xisserve

GTK2 application launcher (kickoff/krunner-style), companion to
`xispanel`'s `xisserve` widget.

**Start here if you're picking up xisserve itself: read `PROTOCOL.md`
first.** It documents the exact argv contract the `xispanel` widget side
already implements and ships with -- xisserve's own argument parsing
must match it, not the other way around.

## Status

Implemented: argv-driven positioning/theming, flock singleton with a
control-socket relay for reposition+retheme+toggle on a second
invocation, quits itself if the xispanel that launched it dies, a
categories pane (left) + results pane (right) split that collapses to a
flat full-width search across every app while typing, favorites
(right-click a result), icons (app icons plus a plugin's own), and a
footer of confirm-before-running power actions (shutdown/reboot/
suspend/logout/switch-user/lock), each shown only if its backend is
actually installed.

Not yet implemented: an icon-grid layout (list-only for now).

## Why a separate process

Same reasoning as `xisnotif` (see its README): a real search-as-you-type
popup with icon-grid/list results is GTK-shaped UI work that doesn't fit
`xispanel`'s "no toolkit, plain Cairo" philosophy. `xispanel`'s own
`launcher` widget (`../xispanel/widgets/launcher.c`) stays a simple
pin-a-shortcut icon; `xisserve` is where full application search lives.

## Positioning

`xispanel`'s `xisserve` widget (`../xispanel/widgets/xisserve.c`)
invokes `xisserve` with its own on-screen anchor coordinates, panel
edge/output geometry, and theme (colors/font) as argv flags, not
embedding/reparenting -- reparenting would make `xispanel` an
XEmbed-style host for a second toolkit, real complexity for something
that's a floating popup on top of everything anyway, not literally
inside the panel bar. **See `PROTOCOL.md` for the exact flag set and the
singleton/toggle behavior xisserve implements.**

## Search plugins

Typing in the search box searches every installed app by name *and*
runs every enabled plugin against the same query -- a plugin is just a
function (see `xisserve.h`'s `SearchPluginFn`) that gets the query text
and can append its own results, each with its own icon, subtitle
("which plugin"), and click/Enter action. Plugin sources live under
`plugins/`:

- **terminal** (`plugins/terminal.c`) -- if the query's first word
  resolves via `$PATH`, offers "Executar: `<query>`", running it in the
  session's terminal (left open afterwards so a one-off command's output
  doesn't just flash and vanish).
- **globalmenu** (`plugins/globalmenu.c` + `plugins/dbusmenu.c`) -- finds
  the active window's exported application menu (same
  `_KDE_NET_WM_APPMENU_SERVICE_NAME`/`_OBJECT_PATH` mechanism
  `xispanel`'s own `globalmenu` widget uses) and matches every item's
  label against the query, across every submenu depth at once --
  Ubuntu Unity HUD-style ("export" finds Gimp's File > Export As...
  directly, no need to know which submenu it's under). Activating a
  result sends that item's own DBusMenu event, same as clicking it for
  real. Needs `libdbus-1-dev` at build time (falls back to a no-op stub
  otherwise, see the Makefile's `HAVE_DBUS` block) and a session bus at
  runtime.

Any plugin can be disabled via `$XDG_CONFIG_HOME/xisserve.conf` (falls
back to `~/.config/xisserve.conf`), one line per plugin to turn off:

```
PLUGIN	terminal	no
```

(fields are tab-separated, matching `xisback.conf`'s own line shape).
Every plugin not mentioned stays enabled, so a fresh install needs no
config file at all. Reloaded on every open, so an edit takes effect on
the next toggle without restarting the daemon.

## Pages

Besides the launcher view, xisserve opens a specific page when given
that page's mode flag (see `PROTOCOL.md` for who passes what). Sources
live under `pages/`, one file per page, plugged in through a single
table in `xisserve.c`:

- **`--calendar`** (`pages/calendar.c`) -- month view, current month,
  today highlighted, prev/next month and direct year entry. Opened by
  xispanel's `clock` widget.
- **`--audio`** (`pages/audio.c` + `pages/pulse.c`) -- a mixer: the
  streams currently playing/recording with per-application level and
  mute, plus output and input devices with level, mute, default, and
  active. Opened by xispanel's `volume` widget.

### Audio: no libpulse/libpipewire dependency

`pages/pulse.c` shells out to `pactl` rather than linking a sound
server's client library -- the same call `xispanel/pulse.c` already
made. `pactl` ships with `pulseaudio-utils` and is also provided by
`pipewire-pulse`, so one code path covers both; there is nothing to
detect at build time and nothing to `ifdef`. libpulse is built around a
persistent async connection with its own callback mainloop rather than
one-shot calls, so linking it would mean folding part of that mainloop
into GTK's for no gain here.

With `pactl` missing, or present with no server answering, the page
shows an explanatory placeholder and everything else keeps working.

**ALSA-only systems** land on that placeholder, and are told why. It
isn't an oversight: without a sound server there are no per-application
streams to mix at all (that concept is exactly what a sound server
adds), and ALSA's "default device" is a config-file matter rather than
something switchable at runtime -- so the mixer, the default switching,
and the enable/disable controls all have nothing to act on. A limited
`amixer`-based fallback (card list + master/capture level and mute)
would be possible, but it would cover only the parts this page is least
about, for a niche that keeps shrinking as distributions default to
PipeWire.

Per-page settings live in the same `xisserve.conf`, under the page's own
section:

```
AUDIO	scroll_step	5
```

`scroll_step` is how many percent one scroll notch (and one arrow-key
press) moves a volume slider. Default 5.

## Icons

Both app icons (`.desktop` `Icon=`, resolved via `GtkIconTheme` for a
themed name or loaded directly for an absolute path) and a plugin's own
per-result icon are resolved through `xisserve_resolve_icon()`
(`xisserve.h`), cached process-wide by icon spec so repeat lookups
(every rescan, every keystroke) are cheap.

## Open question: matching the system theme

Same as `xisnotif` -- see its README's "Open question" section. Applies
here identically (font + KDE Plasma color scheme in GTK2).

## Building

```
make
./xisserve
```

Needs `gtk+-2.0` and `x11` (`pkg-config --exists gtk+-2.0 x11`).
`libdbus-1-dev` is optional (see "Search plugins" above).
