# KiDesktop

A complete X11 desktop environment for Linux, built from small independent
programs.

## What this is

I daily-drive Plasma 5, and I like it. This is an attempt to rebuild the parts
of that setup I actually use -- window manager, panel, launcher, notifications,
wallpaper, settings, hotkeys -- as a handful of small C programs that start
fast, stay small in RAM, and don't pull in a whole desktop framework to draw a
taskbar.

It began as a single tool: **xnsguard**, a permission guardian for
[XiS](https://github.com/kiyoshispreclerg/xserver), my soft fork of
XLibre/Xorg. That tool became `xisguard`, and then I kept adding the piece I
was missing next, until it turned into a desktop. The old xnsguard repository
is archived [here](https://github.com/kiyoshispreclerg/xnsguard); everything
since lives in this one.

KiDesktop is built with XiS in mind, but it isn't exclusive to it. Only
`xisguard` needs XiS specifically. Everything else targets plain X11 and runs on
any X server.

## About the lightness goal

Being honest about it: the desktop itself is light. Most components are plain
Xlib/XCB + Cairo + Imlib2, no toolkit. The only GTK2 piece (`xisserve`) exists
only where reimplementing certain widgets in raw Cairo wasn't worth it.

But the moment you open a browser, or anything Electron, or a modern GTK3/Qt6
app, all that saved memory is gone several times over. So the lightness is not
really *justifiable* as a system-wide win -- it's just the part I can control,
and it's nice that the shell itself stays out of the way.

## About AI

My other self writes React Native and PHP for a living. Our C is basic. This 
whole thing is written with heavy AI assistance -- but we read every line, 
test it, and we're the one deciding what gets built and how it fits together.
The architecture decisions, the protocols between components, and every "no,
do it this other way" are ours. The C is a collaboration.

## Status

Far from finished, but somewhat usable -- I'm running parts of this repo on my
desktop while writing this. Expect rough edges, missing features, and things that
will change. Everything is in English (code, configs, docs), although I'd prefer
other languages :)

## Components

### Session and windows

**[kisession](kisession/)** -- session leader and service supervisor. Runs and
restarts the desktop's other daemons, then XDG autostart.

**[kiwm](kiwm/)** -- the window manager. Stacking, XCB, Cairo + Imlib2
decoration, multi-monitor with per-output desktops. The WM this desktop runs
on daily.

**[kicomp](kicomp/)** -- optional compositor for `kiwm`. XRender or OpenGL
(GLX/EGL) backends, real transparency, shaped corners, effects. `kiwm` never
depends on it.

### Shell

**[xispanel](xispanel/)** -- panel/taskbar daemon. No toolkit, multiple
panels, any edge, any output. Tasklist, tray, global menu, folders,
notifications, system monitor and more widgets.

**[xisserve](xisserve/)** -- application launcher and misc settings pages
(network, storage, audio, energy, calendar...), kickoff/krunner style. GTK2,
launched on demand by `xispanel`.

**[xisback](xisback/)** -- wallpaper daemon. One wallpaper layer per (output,
desktop), static image or slideshow.

### Settings

**[kiconf](kiconf/)** -- the settings app: appearance, shortcuts, screens,
input, energy and permissions.

**[kiconfd](kiconfd/)** -- settings daemon, the other half of `kiconf`.
Applies the central config to every toolkit the desktop cares about and
reloads on SIGHUP.

**[xismenu](xismenu/)** -- application menu registrar, so Qt/KF5 global menus
work without a full DBusMenu implementation on the window manager's side.

**[xiskeys](xiskeys/)** -- global hotkey daemon for stateless actions: run a
command, media keys, brightness, screenshot, lock, power.

### XiS-specific

**[xisguard](xisguard/)** -- the original tool, and the only component that
requires XiS. Talks to the **Xnotify** X extension to arbitrate privileged
client requests in real time -- allowing, asking, denying or killing based on
your rules.

## Building

For the full suite, from the repo root:

```sh
./configure   # checks shared dependencies (cairo, pango, imlib2, dbus-1, gtk2)
make install
```

Each component also builds on its own, skipping `./configure`:

```sh
cd kiwm && make
```

## License

See [LICENSE](LICENSE).
