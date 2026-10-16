**English** · **[Português (Brasil)](README.pt-BR.md)**

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
whole project is written with heavy AI assistance. In the beginning we read
every line, but after a lot of testing, we came to the conclusion that AI is
very trustable when the human (or even me, an "artificial human") knows what
he wants, and it's not very different from using a DE like KDE, as it also has
a lot of code that we don't read before installing a compiled package; at least
in our code we're the one deciding what gets built and how it fits together. The
things we use, the architecture decisions, the protocols between components, and
every "no, do it this other way" are ours. The code is a collaboration.

## Status

Far from finished, but somewhat usable -- I'm running parts of this repo on my
desktop while writing this. Expect rough edges, missing features, and things that
will change. Everything is in English (code, configs, docs), although I'd prefer
other languages :)

## Components

KiDesktop ships as two groups, built and packaged separately:

- **core** -- the desktop itself (everything under *Session and windows*,
  *Shell*, *Settings*, *History and clipboard* and *XiS-specific* below).
  Package `kidesktop-core`.
- **base** -- the everyday applications (*Applications* below). Package
  `kidesktop-base`, which depends on the core. Heavy libraries are only
  recommended: each app runs without them, with the matching features off.

`kidesktop-core` also stands in for the old single `kidesktop` package.
`apt install ./kidesktop-core_*.deb ./kidesktop-base_*.deb` installs both.

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

**[kitheme](kitheme/)** -- one widget theme for GTK2, GTK3, GTK4, Qt5 and Qt6,
configured in `kiconf`. A small C renderer behind a GTK2 engine, a Qt style
plugin and generated GTK3/4 CSS; presets like Oxygen, classic Windows and flat.

**[xismenu](xismenu/)** -- application menu registrar, so Qt/KF5 global menus
work without a full DBusMenu implementation on the window manager's side.

**[xiskeys](xiskeys/)** -- global hotkey daemon for stateless actions: run a
command, media keys, brightness, screenshot, lock, power.

### History and clipboard

**[kimemory](kimemory/)** -- clipboard history. Records every copy with its
source window, app and document, served back through `xisserve --clipboard`
or the `kimemory` CLI.

**[kistory](kistory/)** -- desktop activity history, as plain searchable
text: windows, focus, files, outputs, audio and xisguard events. No
screenshots, no window contents. `kistory` CLI, or search it from
`xisserve`'s launcher.

### XiS-specific

**[xisguard](xisguard/)** -- the original tool, and the only component that
requires XiS. Talks to the **Xnotify** X extension to arbitrate privileged
client requests in real time -- allowing, asking, denying or killing based on
your rules.

## Applications (base)

**[kiterm](kiterm/)** -- tabbed terminal emulator (GTK2 + VTE): find bar,
colors, transparency, Ctrl+wheel font zoom, sound-theme bell, and a context
menu to send signals to the running program. Needs the GTK2 `libvte-dev`;
without it the build skips kiterm (`./configure` warns).

**[kifiles](kifiles/)** -- file manager (GTK2 + GIO): split view, thumbnails,
devices, undo.

**[kishot](kishot/)** -- screenshots and screen recording, Spectacle-style:
full screen, monitor, active window or region; records with sound, and can
black out chosen windows. On XLibre/XiS it records with a GPU copy that
VAAPI encodes without any readback. The FFmpeg libraries (for its recording
plugin) and libpulse are optional: without them it records through the
`ffmpeg` command, or only takes screenshots.

Planned, not started yet:

- **[kiview](kiview/)** -- image and document viewer.
- **[kitask](kitask/)** -- system monitor, in the spirit of qps.
- **[kipad](kipad/)** -- text editor, in the spirit of Kate.
- **[kipaint](kipaint/)** -- paint program.

## Building

For the full suite (core + base), from the repo root:

```sh
./configure   # checks shared dependencies (cairo, pango, imlib2, dbus-1, gtk2)
make install
```

`./configure` also picks what a plain `make` builds, and remembers it in
`config.mk`:

```sh
./configure --core-only   # the desktop only: no kiterm, kifiles, ...
./configure --with-base   # core + base (the default)
```

Either group can also be built or installed on its own, whatever `./configure`
chose: `make core`, `make base`, `make install-core`, `make install-base`.
Base applications that don't have code yet are skipped.

Each component also builds on its own, skipping `./configure`:

```sh
cd kiwm && make
```

Every build dependency is listed once, in `debian/control`'s
`Build-Depends`; on Debian/Ubuntu `sudo ./scripts/install-build-deps.sh`
installs them.

### Checks and packages

```sh
make lint   # every component with -Werror, cppcheck, .po/.desktop/script checks
make deb    # dist/ kidesktop-core and kidesktop-base .debs for this distro
```

GitHub Actions runs `make lint` on Ubuntu 22.04, Ubuntu 24.04, Debian 12 and Debian 13
for every push and pull request (`.github/workflows/ci.yml`).
*Release packages* (`.github/workflows/release.yml`) builds the source
`.zip` and the `.deb` for those four distros: automatically for a stable
tag or release (`v1.0.0` and up, no `-suffix`), attached to that release;
for a pre-release (`v0.x.y`, `v1.2.0-rc1`) only on demand -- "Run
workflow" with the tag, attached to a release marked pre-release; "Run
workflow" without a tag keeps the files as workflow artifacts. The Debian 12 package also installs on
Devuan 5, and the Debian 13 one on Devuan 6 (excalibur).

## License

See [LICENSE](LICENSE).
