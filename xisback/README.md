# xisback

Wallpaper daemon for KiDesktop. Draws into real
`_NET_WM_WINDOW_TYPE_DESKTOP` windows so compositing window managers
(KWin included) have something to composite for the desktop layer --
without one, a compositor just clears that area to black instead of
reading the root pixmap the way feh/xwallpaper/nitrogen expect.

One wallpaper "layer" per `(output, desktop)`:

- **output** -- `*` (spans the whole virtual screen) or an XRandR output
  name, sized/positioned to exactly that output's CRTC geometry.
- **desktop** -- `*` (sticky, shown on every virtual desktop) or a 0-based
  virtual desktop index; the window manager shows/hides it on desktop
  switch, xisback does not track switches itself.

A layer's source is a single image (static), a directory (slideshow,
cycled every `--interval` seconds, alphabetical or `--shuffle`), or empty
(solid `--color`, also the fallback whenever an image is missing or fails
to decode).

Only one instance runs per session; further invocations talk to it over a
Unix socket instead of spawning a second process. `kiconf`'s Wallpaper tab
is a plain client of that socket. See [PROTOCOL.md](PROTOCOL.md) for the
wire format.

## Usage

```
xisback [--output NAME] [--desktop N] --mode static|slideshow
        [--interval SECS] [--shuffle] [--fade MS] [--color #rrggbb] [PATH]
xisback --clear --output NAME --desktop N
xisback --clear-all
xisback --list
xisback --next --output NAME --desktop N
xisback --on-left-click/--on-right-click/--on-middle-click/
        --on-double-click/--on-scroll-up/--on-scroll-down CMD
xisback --get-actions
xisback --lazy on|off
xisback --get-lazy
xisback --quit
```

With no arguments, it comes up as an empty daemon waiting for commands.

## Memory of hidden desktops

Every layer's image is a full-output pixmap in the X server (VRAM on a
GPU-backed server). Layers of desktops you are not on stay loaded so a
switch shows them instantly. Under kicomp with `keep_hidden_contents` on,
the compositor already keeps its own picture of each hidden layer (for
expo, the cube and desktop switches); xisback then uses that picture as
the window's background and frees its own copy, so each hidden wallpaper
is held once instead of twice. That costs no CPU and changes nothing on
screen.

`--lazy on` also drops the image of hidden layers nobody keeps a picture
of (no compositor, or `keep_hidden_contents=0`) and decodes it again when
the layer is shown: less memory, but more CPU per switch and a brief
flash of the layer's `--color` until the image is ready. Off by default;
the setting is saved in `xisback.conf`.

On a scaled output (the CRTC's transform makes its logical footprint
smaller than its mode) and with an X-DENSITY manager present
(`_X_DENSITY_MANAGER_S<n>`, kicomp), each layer of that output is drawn
at the output's real pixels and published as `_X_DENSITY_PIXMAP`
(TESTS/X-DENSITY.md) instead of being the window's background; the
window keeps just its `--color`. It is one copy at the screen's own
resolution, sharp rather than magnified. kicomp offers that same pixmap
as the hidden layer's kept picture (`_KICOMP_STOWED_PIXMAP`), so with
`--lazy on` a hidden layer kicomp keeps is not unloaded either; one it
doesn't keep is unloaded about a second and a half after it is hidden.
`*` layers, which may span outputs of different scales, stay at 1x.
