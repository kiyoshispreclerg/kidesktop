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

**[kisession](kisession/)** -- session leader and service supervisor. The
display manager tracks this process, not the window manager, so a WM crash or a
deliberate `--replace` doesn't kill your session. Supervises the desktop's own
services with restart backoff, then runs XDG autostart once those are actually
up and answering.

**[kiwm](kiwm/)** -- the window manager. Stacking, plain XCB (no Xlib), Cairo +
Imlib2 for decoration, RandR for multi-monitor with per-output desktops.
Complete enough to be the WM this desktop runs on daily.

**[kicomp](kicomp/)** -- optional compositor for `kiwm`. XRender (no OpenGL
yet), real transparency, shaped corners, fade/scale/geometry effects. Always
optional -- `kiwm` never depends on it.

### Shell

**[xispanel](xispanel/)** -- panel/taskbar daemon. No toolkit: Xlib for
windowing/RandR, Cairo for rendering, Imlib2 for images. Multiple panels, any
screen edge, any output. Widgets for tasklist, tray (StatusNotifier), global
menu, folders, notifications, system monitor and more.

**[xisserve](xisserve/)** -- application launcher, kickoff/krunner style. GTK2,
launched on demand by `xispanel`'s widget. Categories + search, favorites,
icons, and power actions that only appear if their backend is installed.

**[xisback](xisback/)** -- wallpaper daemon. Draws into real
`_NET_WM_WINDOW_TYPE_DESKTOP` windows so compositing window managers have
something to composite, instead of the root pixmap trick that compositors
ignore. One wallpaper layer per (output, desktop), static image or slideshow.

### Settings

**[kiconf](kiconf/)** -- the settings app. GTK2, replacing an earlier
Python/Qt configurator. Edits appearance, shortcuts, screens, input and
permissions.

**[kiconfd](kiconfd/)** -- settings daemon, the other half of `kiconf`. Reads
one central config file and applies it to every toolkit the desktop cares
about -- Xcursor, GTK2/3/4, Qt, icon themes, fonts, colors -- and reloads on
SIGHUP without restarting.

**[xiskeys](xiskeys/)** -- global hotkey daemon. Owns root-window key grabs for
stateless actions only: run a command, media keys, brightness, screenshot,
lock, power. Hotkeys that need another daemon's live state stay in that daemon.

### XiS-specific

**[xisguard](xisguard/)** -- the original tool, and the only component that
requires XiS. Talks to the **Xnotify** X extension to arbitrate privileged
client requests in real time -- clipboard, screen capture, input injection,
hotkey grabs -- allowing, asking, denying or killing based on your rules.

## Building

Each component builds on its own:

```sh
cd kiwm && make
```

## License

See [LICENSE](LICENSE).
