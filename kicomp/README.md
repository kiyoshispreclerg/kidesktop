# kicomp

kiwm's optional compositor, following `kiwm/kiwm-kicomp-projeto.md`.

It started as the Fase 5 prototype — exactly the scene you would see with
no compositing, with real transparency (32-bit windows' alpha and
`_NET_WM_WINDOW_OPACITY`) as the only difference. It now also has:

- window shapes applied while compositing (rounded corners, shaped clients);
- configurable shadows, different for focused and unfocused windows;
- an effect interface: *geometry change*, *fade in/out*, *scale in/out*;
- a per-output frame clock driving the animations;
- configuration in `kicomp.conf`.

No OpenGL yet — the renderer is XRender, which handles translation, scale
and alpha. Wobbly and blur are what will ask for GL.

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
| `--renderer=NAME` | `auto` \| `xrender` |
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
renderer           = auto  # auto | xrender
presenter          = auto  # auto | copy
single_drawable    = 0     # 1 = legacy mode, one drawable for the screen
skip_wm_layers     = 0     # 1 = don't composite kiwm's OSD/wireframe

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

### What's missing, and what each one needs

The effects below all fit XRender — none of them needs GL — but one piece
of infrastructure is still absent:

**(a) a window that outlives its own end** — **done**, along with
`fade-out`/`scale-out`: `window_retain()`/`window_release()`, and an
entry that becomes a *zombie* when X destroys the window while an effect
is still drawing it (the pixmap is ours until we let go).

**(b) a source crop on the scene node.** A `CompRect` saying "draw only
this part of the pixmap", with no scaling. It is what shade needs to roll
up without distorting.

| effect | needs | how |
|---|---|---|
| `minimize`/`restore` | — | scale between the window's geometry and `_NET_WM_ICON_GEOMETRY` (the little box the taskbar publishes on the client window) |
| `shade`/`unshade` | (b) | animated crop of the height, with no scaling and no distortion; detected through `_NET_WM_STATE_SHADED` on the client |
| `desktop-wall` | (a) | + grouping windows by `_NET_WM_DESKTOP` and reading `_KIWM_OUTPUT_DESKTOP` to know about the per-output switch; translation of the whole scene, with `docks` optional (default: they come along) |

`desktop-wall` is the only one that moves more than one window at a time
— the interface already supports that (an effect is not required to have
a `window`) — but it needs the windows of the desktop being left to keep
existing, which is item (a) again.

## Pacing

Each output has its own frame clock (`src/scheduler.c`), running at *its*
refresh rate: a 144 Hz monitor never waits for a 60 Hz one, and the
animation state comes from the shared monotonic clock while each output
merely samples it at its own rhythm.

It does two things: it collapses bursts of damage into one frame, and it
keeps animations at each screen's rate. An output whose deadline has
already passed paints immediately, so an isolated event never waits.
There is still **no** MSC/UST — the period comes from RandR's reported
rate, not from presentation feedback — so it paces and coalesces but
doesn't yet lock to vblank; when the presenter can report a real MSC,
only `scheduler_tick()` changes.

## What is implemented

| Section of the doc | State |
|---|---|
| 17/30/45 — capability detection | Composite/Damage/XFixes/Render/RandR detected at runtime; nothing assumes XiS |
| 4/18 — output as the unit of presentation | one pixmap + picture per output, sized to it, never one global surface |
| 39 — per-output dirty state | only the output damage actually touched is repainted |
| 26 — window crossing outputs | `window ∩ output` clipped per output, one scene node in each |
| 21 — scene graph | intermediate `CompScene`/`CompSceneNode`; effects never see X windows |
| 28 — renderer abstraction | `CompRenderer` vtable, `xrender` backend |
| 15/16 — presenter abstraction | `CompPresenter` vtable, `copy` backend (overlay window) |
| 33 — visual mirror | state comes only from X events; the WM stays the authority |
| 38 — lightness | sleeps in `poll()`, no timers, no polling, no repainting just in case |
| — | window shapes applied as a clip (rounded corners, clients with their own shape) |
| — | real alpha: the client's *and* kiwm's frame in a 32-bit visual, plus `_NET_WM_WINDOW_OPACITY` |
| 22 — transform | 4x4 matrix on the scene node; the XRender backend consumes the affine 2D part |
| 23/42 — effects as modules | their own vtable; a new effect = one file + one line |
| 19 — per-output scheduler | a frame clock per output, at each one's rate |
| 20/40 — time-based animation | progress comes from the monotonic clock; duration is a multiple of a global unit |
| 24.1/24.2/24.3/24.4 — effects | fade in/out, scale in/out (configurable origin) and geometry change |
| — | semantic events (open/close/minimize/maximize/shade/focus/...), configurable per effect |
| — | window-type filter (`windows=`) and several instances of one effect, each with its own parameters |
| — | per-effect easing, spring included |
| — | shadows (nine-patch, cost independent of window size), with their own values for focused and unfocused windows |
| — | windows retained past their own end (`window_retain`), which is what makes animating a close possible |

## What is **not** implemented (and where it goes)

- **More effects** (sections 24.1-24.7) — shade, minimize, desktop wall,
  wobbly, cube. The first three fit XRender; wobbly (mesh) and blur ask
  for the GL renderer.
- **MSC/UST** (the rest of Fase 7, sections 19/49) — the per-output clock
  exists, but its period comes from RandR, not from presentation
  feedback.
- **The XiS FLIP presenter** (Fase 8) — `CompPresentMode` and
  `CompPresenter::get_msc` already exist for it; `caps.flip_per_crtc` is
  declared `false` on purpose, so no code path can believe in it early.
- **Region-based repaint** — damage today decides *which outputs* to
  repaint, not *which part* of them. An optimization, not an interface
  change.
- **Unredirecting a single output** (a fullscreen window) — the decision
  is per output and fits in the paint loop, but isn't there yet.
- **`kiwm` ⟷ `kicomp` IPC** (section 32) — deliberately absent in the
  first version. When it exists it replaces only the *source* of the
  updates; the mirror in `window.c` stays as it is.
- **Per-output X-Density** (section 56) — the compositor doesn't scale
  the scene by density yet.

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
  effects/            one file per effect: geometry, fade-in, fade-out,
                      scale-in, scale-out
  renderer.h          renderer vtable
  renderer-xrender.c  XRender backend
  presenter.h         presenter vtable
  presenter-copy.c    COPY backend (overlay window)
tests/
  argb-window.c       ARGB test client
```
