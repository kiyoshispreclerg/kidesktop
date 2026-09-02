# kicomp

kiwm's optional compositor, following `kiwm/kiwm-kicomp-projeto.md`.

It started as the Fase 5 prototype — exactly the scene you would see with
no compositing, with real transparency (32-bit windows' alpha and
`_NET_WM_WINDOW_OPACITY`) as the only difference. It now also has:

- window shapes applied while compositing (rounded corners, shaped clients);
- configurable shadows, different for focused and unfocused windows;
- an effect interface: *geometry change*, *fade in/out*, *scale in/out*,
  *shade/unshade*, *minimize/restore*;
- a per-output frame clock driving the animations;
- configuration in `kicomp.conf`.

Two renderers now: **XRender**, which is complete and what `auto` picks,
and a **GLX** one that is new and does not do everything the older one
does yet (see below). Wobbly, blur and the cube are what the GL one is
for — they cannot be expressed in XRender at all.

```sh
make
./kicomp
```

Options:

| option | effect |
|---|---|
| `--replace` | take over from a running compositor |
| `--single-drawable` | legacy mode: **one** drawable for the whole screen instead of one per output |
| `--skip-wm-layers` | don't composite kiwm's own layers (`_KIWM_LAYER`: the alt-tab OSD, the move/resize wireframe) |
| `--effects`, `--no-effects` | turn animations on/off |
| `--anim-ms=N` | global animation unit, in ms |
| `--renderer=NAME` | `auto` \| `xrender` \| `glx` |
| `--presenter=NAME` | `auto` \| `present` \| `copy` |
| `-v`, `--verbose` | detailed log (events, windows, layers, frames) |

Every option has an equivalent key in `kicomp.conf` (below); the command
line always wins over the file.

Without `-v` it still prints the essentials: the render and presentation
backends, the detected capabilities, and how many drawables there are and
why — reprinted on every output change:

```
kicomp: kicomp 0.2.1 on :0 screen 0 (3840x1080)
kicomp: renderer=xrender presenter=copy
kicomp: capabilities: composite=1 overlay=1 damage=1 xfixes=1 render=1 randr=1 present=0 flip-per-crtc=0
kicomp: 2 drawables (one per output)
kicomp:   [0] DP-1         1920x1080+0+0 @ 143.98 Hz
kicomp:   [1] HDMI-1       1920x1080+1920+0 @ 60.00 Hz
kicomp: shadows: radius 14/10, opacity 0.45/0.25, offset +0+6/+0+3 (focused/unfocused)
kicomp: effects on, animation unit 160 ms
kicomp:   geometry     on   160 ms out     on: maximize,unmaximize,fullscreen,unfullscreen,move  for: unknown,normal,dialog,utility,toolbar
kicomp:   fade-in      on   160 ms out     on: open,restore,desktop-enter  for: all
kicomp:   fade-out     on   160 ms out     on: close  for: ...
kicomp:   scale-in     off  160 ms out     on: open,restore,desktop-enter  for: ...
kicomp:   scale-out    off  160 ms out     on: close  for: ...
```

with `--single-drawable`:

```
kicomp: 1 drawable (legacy single-screen mode)
kicomp:   [0] screen       3840x1080+0+0 @ 60.00 Hz
```

Legacy mode is the one place where the "one drawable per output" rule is
deliberately switched off — scene, renderer, presenter and dirty state
don't change, they simply get a single screen-sized output.

`kicomp` is optional in every sense: `kiwm` doesn't know it exists, needs
no changes to be composited, and killing `kicomp` returns the session to
the uncomposited path (section 31 of the design document).

## Configuration

`$XDG_CONFIG_HOME/kicomp.conf`, or `~/.config/kicomp.conf`. The file is
optional — every key has a working default. Same format as `kiwm.conf`
(`key = value`, `#` comments), plus sections for shadows and effects:

```ini
# ---- global ----
effects            = 1     # animations on
animation_duration = 160   # the animation unit, in ms
renderer           = auto  # auto | xrender | glx
presenter          = auto  # auto | present | copy
single_drawable    = 0     # 1 = legacy mode, one drawable for the screen
skip_wm_layers     = 0     # 1 = don't composite kiwm's OSD/wireframe

# ---- per-output scaling (HiDPI) ----
# One section per output, by RandR name; [output:*] is the default for the
# ones without a section of their own.
[output:DP-1]
scale = auto               # auto (the DPI property) | a number like 2.0

# ---- shadows ----
[shadow]
enabled  = 1
windows  = windows,menus     # which window types get one
radius   = 14                # blur radius, in pixels
opacity  = 0.45
offset_x = 0
offset_y = 6
color    = #000000
# the same five for unfocused windows; each one left out here repeats
# the focused value
radius_inactive  = 10
opacity_inactive = 0.25
offset_y_inactive = 3

# ---- effects ----
[effect:geometry]
enabled  = 1
duration = 1.0                  # multiple of animation_duration, not ms
events   = maximize,unmaximize,move
windows  = normal,dialog

[effect:fade-in]
enabled  = 1
duration = 1.0
events   = open,restore,desktop-enter
windows  = all

[effect:fade-out]
enabled  = 1
duration = 1.0
events   = close
windows  = all

[effect:scale-in]
enabled  = 1
duration = 1.0
events   = open
windows  = windows,menus
from     = 0.8                  # starting size, fraction of the final one
origin   = window               # window | pointer | output

[effect:scale-out]
enabled  = 1
duration = 1.0
events   = close
to       = 1.15                 # > 1 swells before vanishing
origin   = window

[effect:shade]
enabled  = 1
duration = 1.0
events   = shade,unshade
windows  = windows

[effect:minimize]
enabled  = 1
duration = 1.0
events   = minimize,restore
windows  = windows
fade     = 1                    # fade along the way as well as shrink

[effect:desktop-wall]
enabled  = 1
duration = 1.5                  # the whole screen moves: longer than one window
easing   = in-out               # a pan is weighted at neither end
events   = desktop-leave,desktop-enter
windows  = all
distance = 1.0                  # how far, as a fraction of the output's size
fade     = 0                    # dim on the way out/in as well as slide
crossing = fade                 # the part on another output: fade | hide

[effect:dodge]
enabled      = 0              # off by default: it moves windows you didn't touch
duration     = 1.5
events       = focus
windows      = windows        # which windows may dodge, not which cause it
easing       = in-out         # shapes each half of the swing
strength     = 1.0            # fraction of the distance that would clear the overlap
clearance    = 8              # px of gap left between them once aside
max_distance = 0              # px ceiling; 0 = whatever clearing it takes
raise_at     = 0.5            # when the focused window is allowed forward

[effect:smooth-move]
enabled  = 0                    # off by default: deliberate lag is a taste
duration = 0.35                 # the filter's time constant, not a length
events   = move
windows  = windows
max_lag  = 48                   # px the picture may fall behind the pointer
resize   = 0                    # smooth resize drags too

# a second instance of the same effect, with different numbers: inherits
# everything from [effect:scale-out] and overrides only what it declares
[effect:scale-out:minimize]
events   = minimize
duration = 2.0
to       = 0.2
origin   = pointer
```

### Instances

`[effect:<name>]` configures an effect's **base** instance.
`[effect:<name>:<instance>]` creates another one of the same effect, which
**starts as a copy of the base** and overrides only the keys it states —
that is how "scale-out on close, and a slower, deeper scale-out on
minimize" becomes two sections instead of two effects.

- a specialized instance repeats **nothing**: whatever it doesn't say is
  whatever the base has;
- section order in the file doesn't matter — kicomp reads the file in two
  passes, bases first and instances after;
- to use only the specialized ones, put `enabled = 0` in the base;
- each instance has its own copy of the module's keys (`to`, `origin`,
  ...), so they really can differ;
- the startup log lists them all, already resolved:

```
kicomp:   scale-out              on   200 ms  on: close     for: normal,dialog,...
kicomp:   scale-out:minimize     on   400 ms  on: minimize  for: normal,dialog,...
```

Trailing comments (`origin = pointer  # ...`) and trailing whitespace are
accepted — the `#` just needs a space before it.

**The animation unit.** No effect has a time of its own in milliseconds:
each asks for a *multiple* of `animation_duration` — `0.5` for something
that should feel instant, `1.0` for an ordinary transition, `2.0` for a
big one. One key then speeds the whole desktop up or down coherently,
instead of leaving a pile of independently tuned animations.
`animation_duration = 0` keeps the effects on but finishes all of them
immediately.

Every effect section takes the five universal keys — `enabled`,
`duration`, `events` (which events), `windows` (which window types) and
`easing` (how the movement is weighted) — and past those whatever the
effect itself understands; each module parses its own (`config_key` in
`effect.h`), and a key it doesn't know becomes a warning on the terminal
rather than silence.

## Events

The core doesn't hand effects X transitions (mapped, unmapped,
reconfigured) but **what happened to the window**, in the desktop's own
vocabulary:

```
open   close   minimize   restore   maximize   unmaximize
shade  unshade fullscreen unfullscreen  focus  unfocus  move
desktop-leave  desktop-enter
```

Each effect declares which of them it answers to, and that is
configuration:

```ini
[effect:geometry]
events = maximize,unmaximize,move   # no shade: rolling up is shade's job

[effect:fade-out]
events = close,minimize             # dissolve on close AND on minimize
```

`all` and `none` work as the whole list. The core filters before calling
the module, so an effect is never handed an event the user didn't ask for
— which is what makes "roll up on shade without geometry sliding
underneath it" a config line rather than a special case inside an effect.

## Easing

`easing=` says how a movement between two points is spread over time —
and it works for any effect, because the core is what applies the curve:

| value | shape |
|---|---|
| `linear` | even from start to finish |
| `in` | slow at the start, fastest as it arrives — weight at the origin |
| `out` | fast at the start, easing into the destination — weight at the destination (**default**) |
| `in-out` | slow at both ends, quick through the middle |
| `spring` | overshoots slightly and settles back, like an object with mass |

The CSS names (`ease-in`, `ease-out`, `ease-in-out`) are accepted too.
`spring` is what reads as "slick": a few percent of overshoot, damped
well before the end, and pinned at both ends — the animation still starts
exactly where it started and lands exactly where it belongs.

```ini
[effect:geometry]
easing = spring

[effect:fade-in]
easing = linear     # a curve on a fade fools nobody
```

## Shadows

A shadow isn't an effect — nothing about one animates — so it has a
section of its own, `[shadow]`, and the renderer draws it under each
window.

| key | what it does |
|---|---|
| `enabled` | turns them on (off by default: a shadow is a taste) |
| `windows` | which types get one (same list as the section below) |
| `radius` | blur radius, in pixels (1–64) |
| `opacity` | 0–1 |
| `offset_x`, `offset_y` | offset |
| `color` | `#rrggbb` |
| `*_inactive` | the same five for unfocused windows |

Each `*_inactive` you don't write repeats the focused value — so "the
same shadow, just fainter when unfocused" is one line:
`opacity_inactive = 0.25`.

**Cost.** Low, and independent of window size — which is the usual
objection to shadows on XRender. A Gaussian blur of a rectangle is
separable, and the blur of an *edge* is the same profile all along it: a
shadow is four corner tiles, four one-pixel strips repeated along the
sides, and a solid middle. The tiles depend only on the radius, so they
are built once and reused by every window; per frame a shadow costs nine
composites of a solid colour through a mask. Nothing is recomputed when a
window moves or resizes. (The expensive version of this — a blurred
bitmap the size of the window, rebuilt on every step of a drag — is a
different implementation, not this one.)

**The cut-out follows the shape, not the rectangle.** The window's own
area is taken out of the shadow — an opaque window would cover it anyway,
but on a translucent one the shadow would show *through* the window,
which always looks wrong. Cutting the rectangle would leave a little
notch of missing shadow at each rounded corner (the area inside the
rectangle but outside the window); cutting the real silhouette lets the
shadow reach into the corner. It costs three asynchronous requests more
than the rectangle and no round trip: the region is the one already
cached for clipping the window, and only has to be moved into the
target's coordinates.

**A shaped window casts around its shape.** The shadow surrounds what can
actually be *seen* of the window, not its rectangle. For almost every
window the two are the same; for the few where they aren't, the
difference is the whole story — VirtualBox's mini-toolbar is a
screen-sized window with a small bar shaped out of it, and shadowing its
rectangle drops a full-screen shadow behind the desktop.

**Maximized or fullscreen windows cast none.** A window filling its
screen has nothing to cast onto: its edges are the screen's edges. At
best the shadow is invisible, at worst it is a dark band down the side of
the next monitor.

One limitation that remains: no shadow is drawn while a window is being
transformed by an effect — a shadow sitting still while the window slides
away is worse than no shadow at all.

## Window types

In the same way, `windows=` says **which windows** an effect applies to.
One type per `_NET_WM_WINDOW_TYPE` value, plus `unknown` for windows that
declare no type at all (most older applications):

| value | `_NET_WM_WINDOW_TYPE_…` |
|---|---|
| `unknown` | *(no type declared)* |
| `normal` | `NORMAL` |
| `dialog` | `DIALOG` |
| `utility` | `UTILITY` |
| `toolbar` | `TOOLBAR` |
| `splash` | `SPLASH` |
| `menu` | `MENU` |
| `dropdown-menu` | `DROPDOWN_MENU` |
| `popup-menu` | `POPUP_MENU` |
| `combo` | `COMBO` |
| `tooltip` | `TOOLTIP` |
| `notification` | `NOTIFICATION` |
| `dnd` | `DND` |
| `dock` | `DOCK` |
| `desktop` | `DESKTOP` |

And five group shorthands:

| group | equals |
|---|---|
| `all` | everything |
| `none` | nothing |
| `windows` | `unknown,normal,dialog,utility,toolbar,splash` |
| `menus` | `menu,dropdown-menu,popup-menu,combo` |
| `popups` | `menus` + `tooltip,notification,dnd` |

```ini
[effect:fade-in]
windows = normal,dialog,tooltip,popup-menu
```

kiwm's own layers (`_KIWM_LAYER`: the alt-tab OSD, the wireframe) and
`InputOnly` windows never get an effect — that is a core rule, not
configuration.

**How the core knows.** An X unmap can be a close, a minimize or leaving
a desktop; a resize can be a maximize, a shade, going fullscreen or just
a resize. The difference lives in properties (`_NET_WM_STATE`, `WM_STATE`
on the client window, `_NET_CURRENT_DESKTOP`/`_KIWM_OUTPUT_DESKTOP` on
the root). So classification happens a beat later: the loop drains the
whole queue, and only then does `windows_flush_events()` decide what
happened, with the entire batch in hand.

Half of that is the WM's responsibility: kiwm publishes state **before**
the geometry that carries it out (see `kiwm/client.c`). It reads
backwards, but it is what makes the order of events say what happened —
announced afterwards, `_NET_WM_STATE` arrives after the ConfigureNotify
it explains, and by then the wrong animation is already running (that was
exactly the bug of geometry sliding a window that was being rolled up).
For window managers that don't do this, kicomp still makes a round trip
before classifying, which is the best that can be done from outside.

It is also the heuristic that section 32's IPC will replace: the WM knows
first-hand what it did.

## Effects

The interface is in `src/effect.h` and is deliberately small: the core
knows a running effect (`CompEffect`/`CompEffectOps`: `update`, `apply`,
`finished`, `destroy`) and a module that decides when to start one
(`CompEffectModule`, with one event callback). It never knows *what* the
effect is.

Two rules an effect has to respect:

- **time, not frames** — `update()` gets a monotonic instant and the
  duration comes from the global unit; the same effect takes the same
  time on a 60 Hz monitor and on a 144 Hz one;
- **per output** — `apply()` runs once per output being painted, with
  that output's scene, so the same effect can be mid-flight on one
  monitor and finished on the other.

And one it can never break: **it doesn't touch the WM's logical state**
(section 27). An effect changes a scene node's `transform` and `opacity`,
and nothing else — the window really is where the WM says it is; it
merely looks like it hasn't arrived yet.

Adding an effect = one file in `src/effects/`, its declaration in
`effect.h`, and one line in `effect.c`'s table. Nothing else in the
compositor changes (section 42).

A module also declares the size and defaults of its config block
(`config_size`/`config_defaults`/`config_key`); the core allocates **one
block per instance** and passes the instance that matched (`self`) to the
event callback — which is the entire mechanism behind multiple instances.

### `fade-in` (section 24.1)

A window that has just appeared comes up from transparent. No keys of its
own: just the universal ones. On by default, for
`open,restore,desktop-enter`, on everything but the desktop.

Opacity is *multiplied*, not assigned: a terminal already half
transparent through `_NET_WM_WINDOW_OPACITY` doesn't become opaque just
because it was opening.

### `scale-in` (section 24.2)

A window that has just appeared grows into place. The destination is
always its real geometry; what is configurable is where it grows from:

| key | values |
|---|---|
| `from` | starting size as a fraction of the final one (0.05–4.0, default 0.8; above 1 shrinks into place instead) |
| `origin` | `window` (its own centre, default), `pointer` (where the mouse is), `output` (the monitor's centre) |

The origin is read once, when the effect starts — a `pointer` origin that
followed the mouse would drag the animation sideways. Off by default:
stacked on top of `fade-in` it is a matter of taste, so it is left to be
chosen.

### `fade-out` (section 24.1) and `scale-out` (section 24.3)

The same two reversed, on closing. `scale-out` also reverses the
direction: it starts at the real size and goes to `to`, toward the same
`origin` `scale-in` would have grown out of. A `to` above 1 makes the
window swell slightly before vanishing instead of shrinking. The two
compose without knowing about each other — one writes `transform`, the
other `opacity` — which is section 24.3's "zoom + fade on close".

They are the first effects that outlive their own subject: by the time
they start, X has already unmapped the window and the application may
already be gone. What keeps being drawn is the pixmap the compositor
named while the window still existed, held alive by `window_retain()`
(see `window.h`) until the animation ends. The `release` lives in the
effect's `destroy`, so a cancelled effect — the window came back, the
compositor is shutting down — frees it exactly as a finished one does.

By default they answer only to `close`. To have minimizing dissolve too,
`events = close,minimize` — and when the `minimize` effect exists, take
it back out of that list.

### `shade` / `unshade`

The window rolls up behind its own titlebar, and unrolls back out of it.
No scaling and no distortion: the content is *cropped*, never squashed —
the visible height shrinks to the titlebar (or grows back) while the
pixels that stay visible keep their size, which is what makes it read as
a blind rolling up rather than a window being squeezed.

The two directions need different pixels, which is worth stating:

- **shade** — by the time the compositor is told what happened, the WM
  has already collapsed the frame to its titlebar and unmapped the
  client: the content that has to roll up is gone from the live pixmap.
  It is drawn from the **stash** instead — the contents the resize
  replaced, set aside rather than freed (`renderer_window_stash()`), held
  for as long as the effect runs.
- **unshade** — the frame is full-size again and the client is mapped, so
  the live contents are the right ones; only the crop grows.

Keep `shade` out of `[effect:geometry]`'s events (it is out by default),
or the window will slide and scale underneath this at the same time.

### `minimize` / `restore`

The window scales between where it lives and the little box the taskbar
reserved for it, so it is visibly going *somewhere* rather than just
disappearing. Where it goes comes from `_NET_WM_ICON_GEOMETRY`, which a
taskbar publishes on each client window it lists (xispanel's tasklist
does). With no taskbar saying anything, the window collapses toward the
bottom edge of its own output — a guess, but a better one than the centre
of the screen.

| key | what it does |
|---|---|
| `fade` | fade out along the way as well as shrink (default on) — never all the way to nothing before the end, or the last third of the motion is invisible |

Minimizing draws a window X has already unmapped, so it keeps it alive
with `window_retain()` exactly as `fade-out` does. If `fade-out` is also
answering to `minimize`, turn one of the two off — otherwise both will
animate the same disappearance.

### `geometry` (section 24.4)

The first one. A window that jumps to another size or place — maximize,
restore, half-tile, snap — slides and scales there instead of
teleporting.

It does **not** animate drags: a move/resize with the mouse arrives as a
stream of configures, and animating those would leave the window visibly
behind the pointer. Without section 32's IPC (the WM is what knows a drag
is in progress), the stream itself is the signal — `window.c` times the
gap between configures and marks the sequence as interactive. That is the
heuristic the IPC will replace.

While a window is being transformed the shape clip steps aside: the
region is in untransformed coordinates and XFixes can't scale it, so
rounded corners go square for about a sixth of a second. The GL renderer,
which can transform the mask along with the picture, is where that stops
being a trade.

### `desktop-wall`

Switching desktops slides the windows of the one being left off one side
of the output while the ones being entered slide in from the other, as
though the desktops were panels of a single long wall and the output were
a window onto it.

This is the first effect that moves more than one window at a time, and
it needed nothing new from the interface: the WM unmaps every window of
the outgoing desktop and maps every window of the incoming one, so what
arrives is one `desktop-leave` per window on the way out and one
`desktop-enter` per window on the way in. Each gets its own effect, all
of them with the same duration and the same easing, and the result reads
as one motion because it *is* one motion, described a window at a time.

| key | what it does |
|---|---|
| `distance` | how far a window travels, as a fraction of the output's size (0–4, default 1.0). At `1.0` a window ends exactly one screen away, so the two desktops never overlap; less and they slide over each other; more and they pull apart with a gap of background between them |
| `fade` | dim towards the edges as well as slide (default off) |
| `crossing` | what happens to the part of a window that reaches onto another output: `fade` (dissolves in place, in step with the slide — default) or `hide` (goes at once) |

**One output pans, not the screen.** The wall moves only the output whose
desktop changed. With two monitors side by side that is not a detail: a
window sliding out of the left monitor would otherwise slide *into* the
right one, showing one desktop's transition on another desktop that isn't
going anywhere. Since each output has its own scene and its own drawable
(section 18), confining it is one comparison — the effect simply declines
to touch a scene that isn't its output's.

That leaves the windows straddling the boundary. Their far piece is on an
output that is staying put, so it can't slide (same reason) and it can't
stay (its window is leaving), which is what `crossing=` decides. `fade` is
the default and is also the cheaper of the two motions: a dissolve
repaints one fixed rectangle per frame, while a slide repaints the union
of where the window was and where it went, every frame.

**Which way it slides** comes from the WM, not from a guess. `desktop.c`
reads the current desktop *per output* (`_KIWM_OUTPUT_DESKTOP` with
`_KIWM_OUTPUTS`, see `kiwm/PROTOCOL.md`; plain `_NET_CURRENT_DESKTOP`
under any other WM) and the grid they sit in (`_NET_DESKTOP_LAYOUT`), and
reports the step from the old cell to the new one. Going right, the wall
pans right: the outgoing windows leave to the left and the incoming ones
arrive from the right. Going up, the same thing vertically. With no
direction to be had — the switch was on another output, or the WM
publishes nothing to go by — the effect does not run at all rather than
invent one.

One switch moves exactly one screen, whichever desktop you jump to: the
desktop three cells over is still "to the right", and sliding three
screen widths in one animation would be a tour of the desktops nobody
asked for.

The windows on the way out have already been unmapped by the WM, so the
wall holds them with `window_retain()` exactly as `fade-out` does. Two
things worth knowing about the timing:

- The WM must publish the new desktop **before** the unmaps that carry it
  out, or a compositor sees the windows vanish and classifies them as
  *closed* — the closing animation on a desktop switch. kiwm does
  (`output.c`), the same way it publishes `_NET_WM_STATE` before the
  geometry that carries a shade out. kicomp also re-reads the desktop
  properties before classifying, as the best-effort half for WMs that
  don't.
- If `fade-out`/`scale-out` are also answering to `desktop-leave` (they
  are not by default), they will animate the same departure as the wall.

### `dodge`

When a window is raised and takes focus, the windows that were covering
it step aside — each towards whichever edge is nearest — and settle back
where they were, so the raise reads as things making room rather than as
one rectangle appearing on top of another between two frames.

| key | what it does |
|---|---|
| `strength` | how much of the distance that would fully clear the overlap is actually travelled (0–1, default 1.0 — all of it: a window that only half clears the one it was covering has not made room, it has twitched) |
| `clearance` | pixels of gap left between the two windows once aside (default 8) — stopping exactly at the edge reads as one window stuck to the other rather than as having got out of the way |
| `max_distance` | ceiling in pixels, `0` (the default) for no ceiling — whatever clearing it takes |
| `raise_at` | when the focused window is allowed to come forward, as a fraction of the duration (default 0.5 — the moment the others are fully aside). `0` lets it come forward at once, which is what the WM did |

Direction is the cheapest honest answer: of the four ways out of the
overlap, the one needing the least movement. A window overlapping a little
on the left slides left; one overlapping at the bottom drops down. The
motion is out and back within one duration — a window that stepped aside
and *stayed* aside would be lying about where it is for as long as it kept
it up.

Two things make it unusual. It is the only effect that animates windows
other than the one the event was about: the event arrives for the window
that gained focus, and what gets animated is everything that was
*covering* it, one effect each — which is not the same as everything
below it now: the WM raises and focuses in one gesture, so by the time
anything is classified everything overlapping is below, including windows
that were always behind and never covered anything. The stacking from
before the batch is kept for exactly this question. The interface already allowed
that — an effect names the window it animates, which needn't be the one it
was told about. And `windows=` here filters *which windows may dodge*, not
which may cause a dodge.

Three kinds of window are left alone, all for the same reason — they are
not in anyone's way:

- **always-on-top** windows (`_NET_WM_STATE_ABOVE`): they are in front by
  the user's own instruction and will still be in front afterwards, so
  moving one aside would be getting out of the way of something it is not
  in the way of;
- **the focused window's own siblings**: an application is often several
  windows — a main window and its dialogs, VirtualBox's machine window and
  its detached mini-toolbar — and those are one thing on screen. Grouping
  is read from `WM_TRANSIENT_FOR`, `WM_CLIENT_LEADER` and `_NET_WM_PID`, in
  that order of confidence, once per window;
- **every window, when the focus came with the window appearing**: a window
  that just opened or was restored takes focus as a matter of course, and
  nothing was covering it a moment ago because a moment ago it wasn't
  there. The core marks that case on the event itself
  (`CompEvent::with_appear`) rather than leaving the effect to guess from
  timing.

Off by default: it moves windows the user did not touch, which is a strong
opinion for a compositor to have without being asked.

### `smooth-move`

A window being dragged is drawn a little behind where the pointer has
actually put it and catches up continuously, so a stream of configures
arriving in uneven steps — which is what a drag always is — reads as one
smooth glide instead of a series of small jumps.

This is the case `geometry` deliberately refuses, turned into a feature,
with two differences that matter:

- **The lag is a filter, not an animation.** There is no "from" and "to"
  to travel between: there is an offset between where the window looks
  like it is and where it really is, and that offset decays towards zero
  the whole time. Every configure adds to it, the decay eats it, and the
  picture is always converging on the truth instead of replaying a path
  towards a destination that has already changed.
- **The offset is capped.** Smoothing is worth a few pixels of lag and no
  more: past that the titlebar visibly separates from the pointer holding
  it, which reads as the compositor being slow rather than the window
  being smooth. `max_lag` is that ceiling, and the window never falls
  further behind however fast the drag is.

| key | what it does |
|---|---|
| `duration` | the filter's time constant τ, as the usual multiple of the global unit (default 0.35): how long the picture takes to close about two thirds of the gap. Short is a de-jitter, long is a visible glide |
| `max_lag` | how far the picture may fall behind, in pixels (0–400, default 48) |
| `resize` | smooth resize drags as well as moves (default off — the frame would be drawn at a size the client hasn't painted yet) |

The decay is exponential and computed from elapsed time, never from a
frame count: averaging the last N frames' positions, the obvious way to
write this, quietly assumes the frames are evenly spaced, and kicomp's
are not (a frame happens when something is dirty). `offset *= exp(-dt/τ)`
is what that average converges to once you stop assuming it, and it costs
one multiply.

Off by default: it is the one effect that touches something the user is
actively holding.

### What's missing, and what each one needs

The effects still to come all fit XRender — none of them needs GL.

Two pieces of infrastructure they were waiting on now exist:

- **a window that outlives its own end** — `window_retain()` /
  `window_release()`, and an entry that becomes a *zombie* when X
  destroys the window while an effect is still drawing it (the pixmap is
  ours until we let go). What `fade-out`, `scale-out` and `minimize` are
  built on.
- **the stash** — the contents a resize replaced, kept instead of freed
  (`renderer_window_stash()`), so an effect can draw what the window
  looked like a moment ago. What `shade` is built on, since the frame has
  already collapsed to its titlebar by the time anyone knows it was a
  shade.

What's left:

| effect | how |
|---|---|
| `cube` | the wall's rotation instead of its translation: needs a perspective transform the XRender backend can't express (its transform is affine), so this one waits for GL |
| `wobbly`, `blur` | need the GL renderer: a mesh per window, and shaders |

## Per-output scaling (HiDPI)

An output can be *scaled*: the compositor draws a logical desktop smaller
than the monitor's scanout and magnifies it into the panel's real pixels.
Two rectangles per output, and the difference between them is the whole
feature (`comp.h`):

| | |
|---|---|
| `rect` | the **logical** box: where windows live, in root coordinates. The scene, the effects, the damage and the WM work in these and nothing else |
| `physical` | what the CRTC actually scans out |
| `scale` | `physical / logical`; `1.0` is every output that isn't scaled, and then the two rectangles are the same |

`scale = auto` (the default) reads the per-output RandR property the
XLibre fork publishes, literally called `DPI`, where **96 = 1x, 192 = 2x**
and so on (`TESTS/DPI-PER-OUTPUT.md`). A number in the config overrides it
per output. Below 1 is refused: this only ever *shrinks* the logical
desktop — growing it is `xrandr --scale`'s job, and RandR already confines
the cursor correctly for that case.

**The DPI is followed live.** kicomp subscribes to RandR's output-property
notifications, so `xrandr --output DP-1 --set DPI 192` retimes everything
at once: the outputs are rebuilt, the cursor confinement is re-applied to
the new logical box, and every window is re-asked for the density that
scale wants. No restart, and no polling.

**Confinement is released even on a crash**, and not by anything kicomp
does: the extension makes a confinement owned by the client that set it,
so closing the connection — cleanly or otherwise — drops it. Verified by
setting one from a throwaway client and letting it exit: the next client
reads `active=0`. The explicit release on shutdown is for the tidy case
and for outputs that stop being scaled while kicomp keeps running.

**It requires X-INPUT-SCALE, and without it kicomp scales nothing** —
whatever the config or the DPI property say. The reason is the pointer:
on a scaled output the pixels outside the logical box exist only as the
magnified image, and a cursor that can wander into them is in a part of
the screen no desktop is being drawn into. Confinement can't be done from
outside the server (cursor motion runs through the input pipeline on every
event; a client warping the pointer afterwards would be visibly late),
which is exactly what that extension exists for — one rectangle per CRTC,
no coordinate remapping. Capability decides, never a guess about which
server this is: `input-scale=0` in the startup line means nothing will be
scaled on this machine.

The renderer draws the logical scene **into a physical-sized target**,
magnifying it there rather than magnifying a finished logical frame
afterwards. That distinction is the point: it leaves room for a client
that redrew its own contents densely (X-DENSITY) to land sharp, instead of
being resampled down into a smaller intermediate and blown back up. Shadow
blur radii are scaled with everything else, so a 12 px shadow is 24 real
pixels of gradient on a 2x output rather than a stretched 12.

Window shapes need one extra step there: XFixes has no scale operator, so
on a scaled output the cached shape region is fetched and rebuilt at the
right size (once per shape change, never per frame). At scale 1 not a
single extra request is sent.

### X-DENSITY: the sharp half

Scaling alone magnifies what the client drew at logical size, which is
exactly as sharp as it sounds. X-DENSITY is how it stops being blurry:
kicomp asks a window to redraw its contents at the output's scale into an
auxiliary pixmap of its own, and samples that instead
(`TESTS/X-DENSITY.md`). The window's geometry never changes — same size in
the WM's layout, same decoration, same focus, same everything. Only the
pixels are denser.

Three ordinary properties, no extension needed:

| | | |
|---|---|---|
| `_X_DENSITY_REQUESTED` | kicomp → client | `[num, den]`; deleted means 1/1 |
| `_X_DENSITY_SCALE` | client → kicomp | what it is *actually* drawing at — believed over what was asked |
| `_X_DENSITY_PIXMAP` | client → kicomp | the auxiliary pixmap, logical size × density |

plus the `_X_DENSITY_MANAGER_S<screen>` selection, owned exactly like
`_NET_WM_CM_S<screen>`: a well-behaved client only picks a density other
than 1 while somebody holds it, so killing the compositor leaves every
window drawing itself normally instead of frozen at a density nothing is
sampling. kicomp deletes its requests on the way out as well.

A pixmap raises no Damage of its own, so "I drew a new frame" is the
client rewriting `_X_DENSITY_PIXMAP` with the same XID — every property
change here is therefore also a repaint.

The dense contents are composited *over* the window after it is drawn,
covering the client's rectangle inside the frame: the first pass paints
the frame (decoration, plus a magnified copy of the client area), the
second replaces that middle part with pixels the client really drew at
that size. When the density matches the output scale the second composite
is a 1:1 copy, which is the whole point of rendering into a physical-sized
target.

Verified against the protocol's own reference client
(`TESTS/x-density-client-v2`): it detects the manager selection, gets
`2/1`, redraws 400×300 → 800×600 into its pixmap, and the grid it draws
comes out one pixel wide on screen instead of two soft ones. Killing
kicomp puts it back to 1/1 and leaves no property behind.

**The decoration is dense too**, because kiwm answers the same protocol
for its frames (`kiwm/PROTOCOL.md`). kicomp writes the request on *both*
windows — the client for its contents, the frame for the decoration around
them — and draws two layers over the window: the frame's pixmap, which is
transparent everywhere kiwm didn't paint, and then the client's inside it.
Two drawables published by two programs, each dense on its own account.
Measured on the titlebar of the reference client at 2x: 2.5x the edge
energy of the magnified version.

## The GLX renderer

`renderer = glx` draws the same scene with the GPU. Not for speed —
XRender composites a desktop perfectly well — but for **what can be
expressed**: XRender's picture transform is affine and its clip is a set
of rectangles, which is why a window being animated loses its rounded
corners today, and why wobbly, blur and the cube are not merely slow there
but impossible. A shader has none of those limits.

It follows the same architecture as everything else: **one drawable per
output**. Each output gets its own GLX window, a child of the Composite
overlay covering exactly that output's scanout, and its own swap — so
outputs still never wait for each other and the per-output frame clock
still drives them independently. `presenter-glx.c` is the other half:
with GL, presenting *is* the buffer swap, so renderer and presenter come
as a pair (`presenter=` is ignored when the GLX renderer is chosen).

Windows arrive as textures through `GLX_EXT_texture_from_pixmap` — the
pixmap the compositor already names for each window, bound directly as a
texture with no copy and no readback. That extension is checked for at
startup rather than assumed; without it this backend declines to start.

GLX needs an Xlib `Display` and kicomp is an XCB program, so the backend
opens a second, independent connection used for nothing but GLX. Mixing
the two event queues is a known way to end up with an unexplainable spin;
X resources are server-side and their ids are global, so the windows and
pixmaps the XCB connection owns are perfectly usable from it.

**The effects did not change.** Not one line of `effects/` was touched for
this backend to exist: a scene node is a rectangle, a 4×4 transform and an
opacity (`scene.h`), and those map onto a shader as directly as they map
onto XRender. That was the point of the abstraction, and this is the first
evidence that it holds.

### What it doesn't do yet

`auto` stays on XRender until these are there, and each is a follow-up in
this one file rather than a change anywhere else:

| | |
|---|---|
| shadows | the nine-patch is XRender pictures today; in GL it is a generated texture drawn as nine quads |
| shape clipping | rounded corners — the plan is to draw the window's visible region as one quad per shape rectangle, which is exact and needs no stencil |
| X-DENSITY layers | the dense decoration and contents are drawn by the XRender path only |
| the shade stash | so `shade` has nothing to roll up under this renderer |
| damage | the scissor is the damage region's *bounding box*, not each rectangle, and `GLX_EXT_buffer_age` isn't consulted yet — with a swapchain the buffer being drawn into is not the one presented last frame |
| MSC/UST | `GLX_OML_sync_control` would give the same numbers the Present presenter reports |

Verified on a nested session (llvmpipe, GLX 1.4 direct): the desktop
composites correctly, damage updates reach the textures, windows open with
`fade-in` and `scale-in` running unmodified, and 122 frames of moving and
typing left the process's RSS unchanged with no X errors.

## Damage

Two questions, and they have different answers: *which outputs* to repaint
(the `dirty` flag) and *which part* of each (the damage region).

The region is kept client-side, as a handful of rectangles per output
(`src/region.c`) — deliberately **not** an XFixes region. A region living
on the server is the natural thing for XRender, which can clip a Picture
with it and never needs a round trip, and exactly the wrong thing for
every other backend, which would have to read it back once per frame.
What both need is the answer, not the representation, so the core keeps
rectangles in plain memory and each renderer turns them into whatever its
API wants: an XFixes region here, scissor boxes in a GL backend. Effects
and events post into it through the same `output_damage_rect()` they
already used.

What it buys, measured on a nested 1000×700 session with shadows on and a
client damaging a small area continuously, over 10 seconds of that:

| | X server CPU | kicomp CPU |
|---|---|---|
| whole-output repaint | 670 ms | 20 ms |
| region repaint | 120 ms | 30 ms |

The server is where compositing actually happens, which is why that is
the column that matters; kicomp's own share goes slightly *up*, paying
for the region requests and one round trip per frame.

Three things make it correct rather than merely faster:

- **Collection is batched and lazy.** A `DamageNotify` is only noted;
  nothing is asked of the server until the frame is about to be painted,
  when every waiting window is subtracted and fetched in one go
  (`src/damage.c`). A window damaging itself five hundred times between
  two frames costs what one damaging itself once costs, and a frame costs
  one round trip no matter how many windows changed.
- **The shadow margin.** A shadow is drawn *outside* its window, so the
  area a window's change dirties is bigger than the window.
  `output_damage_rect()` grows every rectangle by the widest shadow reach,
  once, so no caller has to know shadows exist — without it a moved window
  leaves its old shadow behind.
- **Clips compose, they don't replace.** XFixes gives a Picture one clip
  region, so setting a window's shape as the clip would throw the damage
  clip away and repaint that window whole. The backend intersects the two
  (a copy, a translate and an intersect — no round trip), and the same for
  the shadow's own outline.

And one rule that keeps a bug from becoming an invisible one: an output
marked dirty with an *empty* region is repainted whole. Any path that says
"repaint this" without saying where gets a correct frame, never a frame
that quietly paints nothing.

Verified by taking the screen after a series of moves, raises, resizes, a
desktop switch and a window closing, then restarting the compositor for a
freshly composited frame of the same screen: zero differing pixels.

## Pacing and presentation

Each output has its own frame clock (`src/scheduler.c`), running at *its*
refresh rate: a 144 Hz monitor never waits for a 60 Hz one, and the
animation state comes from the shared monotonic clock while each output
merely samples it at its own rhythm. It collapses bursts of damage into
one frame, and an output whose deadline has already passed paints
immediately, so an isolated event never waits.

**The presenter** is how a finished output reaches the screen, and there
are two:

| | how | what it gives |
|---|---|---|
| `copy` | composites the output's target onto the Composite overlay | works on every server |
| `present` | `PresentPixmap` into a CRTC-covering child window of the overlay | the copy happens **at vblank**, and the server reports back *when* the frame landed (MSC + UST) |

`auto` takes `present` when the server has the extension. Each output gets
its own child window covering exactly it, and its frame is presented with
that output's `target_crtc` — so a frame for the 144 Hz monitor is timed
against *that* monitor's vblank, not against whichever CRTC the server
would pick for a screen-spanning window. One window per CRTC is also the
shape a per-CRTC page flip needs later (Fase 8): a window covering exactly
one CRTC can have its buffer scanned out directly.

Frames do not flip yet, and the presenter is not why: an XRender pixmap is
not a scanout buffer, so the server copies it (`mode copy` in the `-v`
log) at the right moment instead of handing it to the display engine.
Flipping is what a GL/GBM renderer unlocks, with this presenter already in
place.

**Throttling.** An output with a frame still in flight is not painted
again — a second frame queued behind the first doesn't appear any sooner,
it just puts one more frame of latency between what the user did and what
they see. The output stays dirty and is painted the moment the completion
arrives, which makes the loop vblank-driven rather than timer-driven while
anything is animating. Measured on a nested session: MSC increments by
exactly 1 between consecutive frames of an animation.

What is still missing from Fase 7 is the other half: the frame clock's
*period* still comes from RandR's reported rate rather than from the UST
timestamps now arriving. Deriving it from those is a change to
`scheduler.c` alone.

## What is implemented

| Section of the doc | State |
|---|---|
| 17/30/45 — capability detection | Composite/Damage/XFixes/Render/RandR detected at runtime; nothing assumes XiS |
| 4/18 — output as the unit of presentation | one pixmap + picture per output, sized to it, never one global surface |
| 39 — per-output dirty state | only the output damage actually touched is repainted |
| 56 — X-DENSITY | the density requested per window on a scaled output, the client's auxiliary pixmap sampled in place of its magnified contents |
| 56 — per-output scaling | logical vs physical box per output, DPI-derived, drawn magnified into a physical target; gated on X-INPUT-SCALE, whose per-CRTC confinement keeps the pointer inside the logical desktop |
| 39 — region repaint | and only the *part* of it that changed: the damage region is tracked per output, clips the background, the windows and their shadows, skips windows nothing touched, and bounds what the presenter copies |
| 26 — window crossing outputs | `window ∩ output` clipped per output, one scene node in each |
| 21 — scene graph | intermediate `CompScene`/`CompSceneNode`; effects never see X windows |
| 28 — renderer abstraction | `CompRenderer` vtable, `xrender` backend |
| 15/16 — presenter abstraction | `CompPresenter` vtable, with `copy` (overlay window) and `present` (PresentPixmap per CRTC, vblank-timed, MSC/UST reported) |
| 33 — visual mirror | state comes only from X events; the WM stays the authority |
| 38 — lightness | sleeps in `poll()`, no timers, no polling, no repainting just in case |
| — | window shapes applied as a clip (rounded corners, clients with their own shape) |
| — | real alpha: the client's *and* kiwm's frame in a 32-bit visual, plus `_NET_WM_WINDOW_OPACITY` |
| 22 — transform | 4x4 matrix on the scene node; the XRender backend consumes the affine 2D part |
| 23/42 — effects as modules | their own vtable; a new effect = one file + one line |
| 19 — per-output scheduler | a frame clock per output, at each one's rate |
| 20/40 — time-based animation | progress comes from the monotonic clock; duration is a multiple of a global unit |
| 24.1/24.2/24.3/24.4 — effects | fade in/out, scale in/out (configurable origin), geometry change, shade/unshade, minimize/restore, desktop wall, smooth move, dodge |
| — | per-output current desktop read from the WM (`_KIWM_OUTPUT_DESKTOP`/`_NET_CURRENT_DESKTOP` + `_NET_DESKTOP_LAYOUT`), which is what gives the wall its direction and tells a departing window from a closing one |
| — | semantic events (open/close/minimize/maximize/shade/focus/...), configurable per effect |
| — | window-type filter (`windows=`) and several instances of one effect, each with its own parameters |
| — | per-effect easing, spring included |
| — | shadows (nine-patch, cost independent of window size), with their own values for focused and unfocused windows |
| — | windows retained past their own end (`window_retain`), which is what makes animating a close possible |
| — | the stash: contents a resize replaced, kept for an effect that still needs them (what shade rolls up) |

## What is **not** implemented (and where it goes)

- **More effects** (sections 24.5-24.7) — wobbly, blur, cube. All three
  ask for the GL renderer, which now exists but is not yet at parity with
  XRender (see above); they come after it is.
- **MSC/UST-derived period** (the rest of Fase 7, sections 19/49) — the
  Present presenter reports MSC and UST per completed frame and the loop
  is throttled by them, but the frame clock's period still comes from
  RandR's reported rate rather than from measured UST intervals.
- **The XiS FLIP presenter** (Fase 8) — `CompPresentMode` and
  `CompPresenter::get_msc` already exist for it; `caps.flip_per_crtc` is
  declared `false` on purpose, so no code path can believe in it early.
- **Unredirecting a single output** (a fullscreen window) — the decision
  is per output and fits in the paint loop, but isn't there yet.
- **`kiwm` ⟷ `kicomp` IPC** (section 32) — deliberately absent in the
  first version. When it exists it replaces only the *source* of the
  updates; the mirror in `window.c` stays as it is.

## Shape and kiwm's layers

**Shape.** Composited, the server clips nothing: `NameWindowPixmap` hands
over the window's whole rectangle, so applying the shape is the
compositor's job. `kicomp` reads the window's `BOUNDING` region
(`XFixesCreateRegionFromWindow`), caches it, and uses it as the target's
clip while that window is drawn, invalidating on `ShapeNotify`/resize. It
is what keeps kiwm's rounded corners round and a window with a shape of
its own (VirtualBox and the like) in the right silhouette.

This does **not** duplicate work with kiwm: kiwm *computes* the shape
(rounded corners, forwarding the client's shape onto the frame) and has
to keep doing it — that is what works with no compositor, and the *input*
shape (clicks) is the server's business either way. kicomp only *reads*
the finished region, once per change, and reuses it every frame.

**kiwm's layers.** kiwm marks its two overlay windows with `_KIWM_LAYER`
(`"osd"`, `"outline"` — see `kiwm/PROTOCOL.md`). With `--skip-wm-layers`
kicomp simply leaves them out of the scene, for when it draws those
transitions as effects itself. kiwm knows nothing about that and doesn't
change behaviour: what to show is the compositor's decision.

(There is no way to "not redirect" just those windows: `RedirectSubwindows`
on the root covers every child, and `UnredirectWindow` only undoes a
*per-window* redirect by the same client. Keeping them out of the scene
is the practical equivalent.)

## Testing

`tests/argb-window.c` is the acceptance client: a 32-bit window filled
with a half-transparent colour.

```sh
cc -o tests/argb-window tests/argb-window.c -lxcb -lxcb-render
./tests/argb-window 300 300 380 260 0.5 0x30a0ff
```

With no compositor the square is opaque; with `kicomp` it blends with
whatever is behind it.

Verified on a 1024x768 Xephyr with `kiwm` + `xterm` + `xclock`:

- the scene is identical to the uncomposited one (decoration, stacking,
  positions);
- `xprop -id <frame> -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 2147483647`
  makes the window 50% translucent;
- `tests/argb-window` **decorated by kiwm** blends correctly with the
  white xterm behind it — that is the test for the ARGB frame on the WM
  side;
- `--override` blends the same way, without going through the WM;
- kiwm's rounded corners and the OSD's own are preserved;
- `xrandr --setmonitor` splitting the screen into two monitors: two
  independent targets, a window crossing the boundary seamlessly;
- `--single-drawable` goes back to a single screen-sized target;
- `--skip-wm-layers` makes the alt-tab OSD and the wireframe disappear
  from the scene (they still exist and work in kiwm);
- with no compositor, the same decorated ARGB window is opaque and
  intact — no regression on the uncomposited path;
- killing `kicomp` hands the screen back to the server with no residue.

## Layout

```
src/
  comp.h              types and global state (KiComp, CompOutput, CompWindow)
  main.c              caps, _NET_WM_CM_Sn selection, overlay, event loop, paint
  config.c/.h         kicomp.conf
  output.c/.h         RandR outputs, per-output targets and dirty state
  window.c/.h         mirror of the window stack (X events only)
  scene.c/.h          per-output scene assembly (window ∩ output clipping)
  transform.c/.h      4x4 matrix and the affine inverse XRender consumes
  animation.c/.h      monotonic clock, easing, the global duration unit
  scheduler.c/.h      per-output frame clock
  effect.c/.h         effect core: running effects + the module table
  shadow.c/.h         shadow configuration and per-window style
  desktop.c/.h        which desktop each output shows, and which way it
                      just moved (the wall's direction)
  inputscale.c/.h     X-INPUT-SCALE: the pointer confined to the logical
                      desktop of a scaled output
  density.c/.h        X-DENSITY: asking clients to redraw densely, and
                      sampling the pixmap they publish
  region.c/.h         the damage region: rectangles, client-side
  damage.c/.h         X Damage in, per-output regions out (batched)
  effects/            one file per effect: geometry, fade-in, fade-out,
                      scale-in, scale-out, shade, minimize, desktop-wall,
                      smooth-move, dodge
  renderer.h          renderer vtable
  renderer.c          the core's side of it: one wrapper per backend hook
  renderer-xrender.c  XRender backend
  renderer-glx.c      GLX backend (GL_EXT_texture_from_pixmap, shaders)
  presenter.h         presenter vtable
  presenter-copy.c    COPY backend (overlay window)
  presenter-present.c PRESENT backend (per-CRTC, vblank-timed)
  presenter-glx.c     the GLX renderer's buffer swap
tests/
  argb-window.c       ARGB test client
```
