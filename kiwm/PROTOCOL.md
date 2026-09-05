# kiwm per-output desktop protocol

Standard EWMH has exactly one global `_NET_CURRENT_DESKTOP`/`_NET_NUMBER_OF_DESKTOPS` pair on
the root window -- there is no concept of "this output is showing desktop 2 while that output
is showing desktop 0". kiwm tracks virtual desktops independently per output (see
`kiwm-kicomp-projeto.md` section 6), so it publishes that state through a small set of custom
root-window properties and one client message, described here. This is the only custom protocol
kiwm exposes right now; everything else it does is covered by standard ICCCM/EWMH (see the
`_NET_SUPPORTED` list kiwm sets on the root window for exactly which of those it implements).

## Model

Outputs are numbered `0..N-1` in the order given by `_KIWM_OUTPUTS`. That index -- not the output
name -- is what every other property/message below uses to refer to an output. Each output has
its own current virtual desktop, numbered `0..NUM_WORKSPACES-1`; `NUM_WORKSPACES` (configurable via
kiwm.conf's `num_desktops=`, default `4`) is a single count shared by every output (no per-output
desktop *count*, only per-output current desktop), exposed as `_KIWM_NUM_OUTPUT_DESKTOPS`.

## Root window properties

### `_KIWM_OUTPUTS` (`UTF8_STRING`, format 8)

The output names, NUL-separated, in index order -- e.g. two outputs named `eDP-1` and `HDMI-1`
are the byte string `"eDP-1\0HDMI-1\0"`. Splitting on `\0` gives you both the output count and
each output's display name in one read. On a machine RandR doesn't report any monitor for (rare,
seen under some nested/synthetic X servers), kiwm falls back to a single output named `default`
covering the whole screen -- there is always at least one entry.

Rewritten (with a `PropertyNotify` on root) whenever outputs are hotplugged.

### `_KIWM_OUTPUT_DESKTOP` (`CARDINAL[]`, format 32)

One `CARDINAL` per output, in the same order as `_KIWM_OUTPUTS`: the current virtual desktop
index (`0..NUM_WORKSPACES-1`) that output is showing right now.

Rewritten (with a `PropertyNotify` on root) whenever any output's current desktop changes, and
whenever outputs are hotplugged (an output keeps its previous desktop index across a hotplug
refresh if a same-named output was already known; otherwise it starts at `0`).

### `_KIWM_NUM_OUTPUT_DESKTOPS` (`CARDINAL`, format 32, single value)

How many virtual desktops each output has (kiwm.conf's `num_desktops=`, default `4`, identical
for every output). Set once at startup; not expected to change at runtime.

### `_KIWM_WM_OUTPUT` (`CARDINAL`, format 32, single value, per client window)

Companion to `_NET_WM_DESKTOP`: the index (per `_KIWM_OUTPUTS` order) of the output this window
currently belongs to. Since `_NET_WM_DESKTOP` alone is only unique *within* an output (see below),
reading both together is what lets a pager reconstruct the `(output, desktop)` cell a window is
actually in -- `output = _KIWM_WM_OUTPUT`, `desktop = _NET_WM_DESKTOP`, and the window is visible
right now iff `_NET_WM_DESKTOP == _KIWM_OUTPUT_DESKTOP[_KIWM_WM_OUTPUT]`.

Set when a window is first managed, and rewritten (with a `PropertyNotify` on the window itself)
whenever it moves to a different output -- dragging it across an output boundary, or a hotplug
that reshuffles output geometry out from under it.

### `_KIWM_MINIMIZED_GEOMETRY` (`CARDINAL[4]`, format 32, per client window)

Where a minimized window's **frame** was when it went away: `x`, `y`, `width`, `height`, in root
coordinates, decoration included. Present only while the window is minimized -- set as it is
minimized, deleted as it is restored, since a visible window's real geometry already says the same
thing.

kiwm itself never needs it (minimizing only unmaps the frame, so it still has the geometry in
memory, which is what the Alt+Tab switcher's outline draws and what restoring puts back). It's
published for everything *outside* kiwm that has to know where a window it can no longer see used
to be: a compositor animating a minimize or a restore from and to the right place, a taskbar doing
the same with its own effects.

### `_KIWM_LAYER` (`STRING`, format 8, on kiwm's own overlay windows)

Marks the override-redirect windows kiwm draws for itself, so a compositor can tell them apart
from application windows:

| value | window |
|---|---|
| `osd` | the window/desktop switcher overlay (osd.c) |
| `outline` | the move/resize/snap wireframe (outline.c) |

Set once when the window is created and never changed. Purely informational: kiwm draws and shows
both exactly the same whether anything reads this or not -- with no compositor running they *are*
the effect, and they must keep working on their own (project doc section 31).

It exists so the compositor can decide not to composite them and draw its own version instead --
a real switcher effect on the composited scene rather than a flat window on top of it. That is
kicomp's `--skip-wm-layers`. The decision belongs entirely to the compositor: kiwm is never told
about it, has no setting for it, and changes no behavior because of it.

## `_XIS_CONFINED_AREA`: kiwm as a consumer

Not a kiwm property -- kiwm **reads** this one. A compositor doing
per-output HiDPI scaling publishes it on the root window: `CARDINAL[4*N]`,
groups of `x`, `y`, `width`, `height` in root coordinates, one per output
whose pointer it has confined (kicomp's `inputscale.c`), and deleted when
nothing is confined.

It names the part of each affected monitor that is really desktop.
Everything outside it on that monitor is scanout the compositor magnifies
the logical desktop into: there is nothing there for a window to be placed
in, and the pointer cannot even reach it.

kiwm shrinks the output to that rectangle in `outputs_refresh()`, matching
by geometry (a rectangle inside an output is that output's) rather than by
name or index, so no agreement with the compositor about naming is needed.
Nothing else in kiwm knows about HiDPI, scaling or densities: maximize,
snapping, placement, the switcher and `_NET_WORKAREA` all work from
`wm.outputs[]` and follow from that one clamp. A `PropertyNotify` on the
root re-reads it, so a compositor starting, stopping or changing an
output's scale is picked up live.

With no such compositor -- and on every server without X-INPUT-SCALE --
the property is simply absent and nothing changes.

## X-DENSITY on frames: kiwm as a client

Not a kiwm protocol -- it is the compositor-to-client density protocol
(`TESTS/X-DENSITY.md` in this tree), and kiwm speaks the **client** side of
it for its own frames.

A compositor doing per-monitor HiDPI scaling draws a logical desktop
magnified into a monitor's real pixels, and everything drawn at logical
size comes out soft. The decoration's pixels are kiwm's: it draws a 26 px
titlebar into the frame, and no compositor can invent detail that isn't
there. So the compositor writes `_X_DENSITY_REQUESTED` **on the frame
window**, and kiwm:

1. re-renders the same decoration through Cairo with `cairo_scale()`
   applied, so the text is re-shaped by Pango at the larger size instead of
   being magnified;
2. into an **ARGB pixmap cleared to transparent** -- kiwm paints the
   titlebar strip and the borders and nothing else, so the client's area
   stays a hole and the app's own contents (dense or not) show through;
3. publishes `_X_DENSITY_SCALE` and `_X_DENSITY_PIXMAP` on the frame,
   contents first and announcement after;
4. rewrites `_X_DENSITY_PIXMAP` with the same XID on every later repaint,
   since a Pixmap raises no Damage of its own and that PropertyNotify is
   the only "there is a new frame here" signal a compositor gets.

With nothing asking, none of this exists: no pixmap, no properties, no
cost. kiwm's own drawing and the uncomposited path are untouched, per the
project doc's section 31.

## Client message: `_KIWM_SET_OUTPUT_DESKTOP`

Send to the **root window** (not to any client window) via `XSendEvent`/`xcb_send_event` with
`SubstructureRedirect | SubstructureNotify` in the event mask, `format = 32`:

```
data32[0] = output index      (0-based, per _KIWM_OUTPUTS order)
data32[1] = desktop index     (0-based, < _KIWM_NUM_OUTPUT_DESKTOPS)
data32[2..4] = 0 (unused)
```

kiwm switches that output's current desktop: windows belonging to the old desktop on that output
are unmapped, windows belonging to the new one are mapped and the first mapped one is focused.
No reply is sent -- observe the effect via the `_KIWM_OUTPUT_DESKTOP` property changing (and, if
that output happens to be the primary one, `_NET_CURRENT_DESKTOP` changing too, see below).

Out-of-range indices (unknown output, or `desktop >= _KIWM_NUM_OUTPUT_DESKTOPS`) are silently
ignored.

## Client message: `_KIWM_PRIME_DESKTOP_LAYERS`

Send to the **root window** the same way, `format = 32`, all five data words zero.

kiwm maps every wallpaper layer that is currently hidden (one whose `_NET_WM_DESKTOP` is not the
desktop its monitor is showing) directly **below** the layer that is on screen for that monitor,
and unmaps it again about 700 ms later. Nothing becomes visible: the wallpaper of the desktop you
are on is opaque and covers that monitor, so the layers held up behind it cannot be seen.

It exists for compositors. X frees an unmapped window's contents, so the only picture there will
ever be of a wallpaper is one taken while it was mapped, and a desktop the user has never visited
has never been mapped -- which is why a compositor's expo grid would otherwise open with the
current desktop's wallpaper and empty cells for the rest. Sending this once at startup (and again
after a new layer appears) gives every hidden layer a moment on screen to be photographed.

A layer on a monitor with nothing on screen to hide behind -- no wallpaper set for the desktop
being shown -- is left alone rather than flashed. Layers on every desktop (sticky) are already
mapped and are not touched. No reply is sent.

## Interaction with standard EWMH

- `_NET_CURRENT_DESKTOP` / `_NET_NUMBER_OF_DESKTOPS` on root mirror the **primary output only**,
  purely so a pager/taskbar that has no idea this extension exists still shows *something*
  sane. Don't use them to drive a per-output pager -- read `_KIWM_OUTPUT_DESKTOP` instead.
- `_NET_DESKTOP_LAYOUT` on root is the `columns x rows` shape the desktops are arranged in
  (`_NET_WM_ORIENTATION_HORZ` from `_NET_WM_TOPLEFT`, i.e. row-major from the top-left), from
  `kiwm.conf`'s `desktop_columns=`/`desktop_rows=`. EWMH makes this a *pager's* property to set,
  but kiwm is where the shape is configured -- its own Meta+Tab grid and its horizontal/vertical
  desktop shortcuts navigate by it -- so kiwm publishes it and a pager can just read it. It
  describes the shape of **one output's** desktops, like every count here.
- Each managed client's `_NET_WM_DESKTOP` is that client's desktop index **within its own
  output** (`0..NUM_WORKSPACES-1`), not a globally unique desktop number. Two windows on
  *different* outputs can both report `_NET_WM_DESKTOP = 0` while genuinely being on unrelated
  desktops -- `_NET_WM_DESKTOP` alone cannot tell them apart.

## How a pager should use this

1. On startup, and again on every root `PropertyNotify` for `_KIWM_OUTPUTS`,
   `_KIWM_OUTPUT_DESKTOP`, or `_KIWM_NUM_OUTPUT_DESKTOPS`, re-read all three.
2. Render `_KIWM_NUM_OUTPUT_DESKTOPS` desktop buttons per output listed in `_KIWM_OUTPUTS`.
   For each output index `o`, highlight the button at `_KIWM_OUTPUT_DESKTOP[o]`.
3. On click, send `_KIWM_SET_OUTPUT_DESKTOP` with that output's index and the clicked desktop
   index.
4. For a per-output tasklist filter: for each window in `_NET_CLIENT_LIST`, read its
   `_KIWM_WM_OUTPUT` and `_NET_WM_DESKTOP`; place it under output `_KIWM_WM_OUTPUT`, desktop
   `_NET_WM_DESKTOP`. It's currently visible iff that desktop equals
   `_KIWM_OUTPUT_DESKTOP[_KIWM_WM_OUTPUT]`. Also watch `PropertyNotify` on each client window for
   `_KIWM_WM_OUTPUT`/`_NET_WM_DESKTOP` changes (the window moved output, or desktop).
