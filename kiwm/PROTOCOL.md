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
for every output). Set once at startup; not expected to change at runtime in this prototype.

### `_KIWM_WM_OUTPUT` (`CARDINAL`, format 32, single value, per client window)

Companion to `_NET_WM_DESKTOP`: the index (per `_KIWM_OUTPUTS` order) of the output this window
currently belongs to. Since `_NET_WM_DESKTOP` alone is only unique *within* an output (see below),
reading both together is what lets a pager reconstruct the `(output, desktop)` cell a window is
actually in -- `output = _KIWM_WM_OUTPUT`, `desktop = _NET_WM_DESKTOP`, and the window is visible
right now iff `_NET_WM_DESKTOP == _KIWM_OUTPUT_DESKTOP[_KIWM_WM_OUTPUT]`.

Set when a window is first managed, and rewritten (with a `PropertyNotify` on the window itself)
whenever it moves to a different output -- dragging it across an output boundary, or a hotplug
that reshuffles output geometry out from under it.

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
