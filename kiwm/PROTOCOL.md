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
its own current virtual desktop, numbered `0..NUM_WORKSPACES-1`; `NUM_WORKSPACES` is currently a
single constant shared by every output (no per-output desktop *count*, only per-output current
desktop), exposed as `_KIWM_NUM_OUTPUT_DESKTOPS`.

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

How many virtual desktops each output has (currently `4`, identical for every output). Set once
at startup; not expected to change at runtime in this prototype.

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

## Interaction with standard EWMH

- `_NET_CURRENT_DESKTOP` / `_NET_NUMBER_OF_DESKTOPS` on root mirror the **primary output only**,
  purely so a pager/taskbar that has no idea this extension exists still shows *something*
  sane. Don't use them to drive a per-output pager -- read `_KIWM_OUTPUT_DESKTOP` instead.
- Each managed client's `_NET_WM_DESKTOP` is that client's desktop index **within its own
  output** (`0..NUM_WORKSPACES-1`), not a globally unique desktop number. Two windows on
  *different* outputs can both report `_NET_WM_DESKTOP = 0` while genuinely being on unrelated
  desktops -- `_NET_WM_DESKTOP` alone cannot tell them apart.

## Known gap: no per-client output property yet

There is currently **no** property exposing which output a given window belongs to. Combined
with the point above, a pager cannot yet, from this protocol alone, group a window into the
right (output, desktop) cell -- only "desktop N of *some* output". Until such a property exists
(a plausible future addition would be a `_KIWM_WM_OUTPUT` `CARDINAL` on each client window,
alongside `_NET_WM_DESKTOP`), the only workaround is comparing a window's geometry (from
`_NET_CLIENT_LIST` + `XGetGeometry`) against each output's rectangle -- doable, but fragile
while a window is mid-drag across an output boundary.

## How a pager should use this

1. On startup, and again on every root `PropertyNotify` for `_KIWM_OUTPUTS`,
   `_KIWM_OUTPUT_DESKTOP`, or `_KIWM_NUM_OUTPUT_DESKTOPS`, re-read all three.
2. Render `_KIWM_NUM_OUTPUT_DESKTOPS` desktop buttons per output listed in `_KIWM_OUTPUTS`;
   highlight the one at `_KIWM_OUTPUT_DESKTOP[output_index]`.
3. On click, send `_KIWM_SET_OUTPUT_DESKTOP` with that output's index and the clicked desktop
   index.
4. For a per-output tasklist filter, see the gap above -- geometry-vs-output-rect comparison is
   the only option today.
