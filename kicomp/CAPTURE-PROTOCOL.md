# kicomp capture protocol

How a screen recorder (kishot first; anything else may follow, the OBS fork
for one) asks the compositor for a picture of an output **with some windows
excluded from it**, and how the two agree on whether that is possible, when it
is needed, which buffer to read, and when it is safe to read it.

*Versão em português: [CAPTURE-PROTOCOL.pt_BR.md](CAPTURE-PROTOCOL.pt_BR.md).*

**Status: specification, protocol version 1, on the `capture-censor` branch.**
Implemented (kicomp 0.3.70, kiwm 0.6.3): the mark and every way to set it (kiwm's
window menu and `capture_exclude_classes`, dialog inheritance, xispanel's
tasklist menu, kishot's "Hide a Window..."); the compositor side in full —
discovery, sessions, `TARGETS`, `STATE`, `HIDE_PID`, the popup rule, the picture
itself on the **XRender** and **GLX** renderers (slots, `REQUEST`/`FRAME`/
`RELEASE`, `UNCHANGED`, fill and hide, per-slot damage so only what changed is
redrawn), and fullscreen windows left unredirected; kishot's side — discovery,
session, seqlock, slots for screenshots, the preview and recordings, `HIDE_PID`
instead of hiding its own window, no list of its own; and the switches
(kiconf's compositor tab). Tested by `kicomp/tests/capture-control.sh`,
`capture-render.sh`, `capture-unredirect.sh`, `kiwm/tests/capture-exclude.sh` and
`kishot/tests/capture-session.sh`, and measured on the real session (GLX, two
monitors). Not implemented: the picture on the **EGL** renderer (its outputs are
`UNAVAILABLE` while an excluded window shows, and kishot censors them itself),
`DMABUF`, and a
test of the effects that draw a window several times (expo, cube, cover-switch,
magic lamp, wobbly): the substitution is at the node, so they are covered by
construction, not exercised. It was written first so the two sides could be
built against the same text.

`slot`, in `FRAME` and `RELEASE`, is the **index** (0-based) into that output's
slot XIDs in `TARGETS` (words 9–12), not an XID.

## Why it exists

Today kishot censors by painting black rectangles over *the window's X
geometry* on its own copy of the root window. That is wrong whenever the
compositor draws the window somewhere else: the cube, expo, the cover
switcher, the magic lamp, wobbly windows, a minimize animation. Nothing the
recorder can read from X says where those pixels went, and a rectangle cannot
follow a deformed mesh. Only the compositor knows, because it is the one
drawing them.

So exclusion belongs to the compositor, as it does in kwin_wayland: **the user
marks a window** (in its title-bar menu, say) **and the compositor produces a
second picture of each output in which that window is not there** (or is a flat
colour). A recorder does not decide what is hidden and does not need a list of
windows; it only asks for the picture and reads it instead of copying the
output.

Non-goals: protecting a window from other capture programs, from the X
server's own `GetImage`, or from the screen itself. Only the picture this
protocol hands out is censored. See [Security](#security).

## Terms

| term | meaning |
|---|---|
| **recorder** | the client taking screenshots or recordings (kishot) |
| **compositor** | kicomp, the owner of `_NET_WM_CM_S<n>` |
| **CM window** | the window that owns `_NET_WM_CM_S<n>`; where the compositor publishes what it can do |
| **output** | one RandR monitor, as in kicomp's "one pixmap per output"; named by its RandR name (`DP-1`) |
| **excluded window** (or censored window) | a window that carries the mark `_KIWM_CAPTURE_EXCLUDE`, or a popup of one; on a reparenting WM, the frame, decoration included (a title bar says what document is open) |
| **session** | one recorder's standing request for censored pictures of some outputs |
| **session window** | a window the recorder creates for the session; carries its request and receives the compositor's answers |
| **slot** | one of a few pixmaps per output the compositor renders censored pictures into |

## Overview

```
user marks a window  ->  _KIWM_CAPTURE_EXCLUDE on it (kiwm's menu, a class rule, a tool)

recorder                                         compositor
   |  GetSelectionOwner(_NET_WM_CM_S0) ---------->  (none: own black rectangles)
   |  read _KICOMP_CAPTURE on the CM window ----->  (absent: own black rectangles)
   |  create session window S, set properties on it
   |  ClientMessage _KICOMP_CAPTURE_OPEN --------->  validate, allocate slots
   |  <---- properties on S + _KICOMP_CAPTURE_STATUS
   |
   |  per frame, per output:
   |    read _KICOMP_CAPTURE_STATE on S
   |    no excluded window visible -> copy the output as always, re-read STATE
   |    one is visible             -> REQUEST ----> final assembly, a second time
   |                                  <---- FRAME (slot, flags)
   |                                  copy the slot
   |                                  RELEASE ---->
   |  ClientMessage _KICOMP_CAPTURE_CLOSE -------->  free slots
```

The cheap case is the common one: while no excluded window is visible on an
output, the screen **is** the censored picture, nothing extra is rendered, and
the recorder reads the output exactly as it does without a compositor — the
direct GPU copy that makes kishot and the OBS fork light **stays**. Only while
an excluded window is showing does the recorder read a slot instead, and a
`CopyArea` from a slot costs it what a `CopyArea` from the root did: both are
pixmaps on the GPU. The extra work is the compositor's, and is small (see the
[cost contract](#cost-contract)).

## Marking windows and policy

### Which windows: the user marks them, with the window manager

A window is **excluded** when either of these holds:

1. **marked**: its client window carries **`_KIWM_CAPTURE_EXCLUDE`** (`CARDINAL`,
   format 32). The value is `1` when the window was marked itself and `2` when it
   inherits the mark because it is a dialog of a marked window. Any non-zero
   value excludes; the difference is for kiwm alone, so that un-marking a window
   clears what it handed down and nothing else. Removing the property un-marks.
2. **popup**: it is an override-redirect window (a menu, a tooltip) whose
   `_NET_WM_PID` equals that of a marked window. This one is **derived by whoever
   reads the marks** — the compositor, and a recorder's own fallback — and is not
   a property, because an override-redirect window is never offered to the window
   manager: kiwm would learn of it only once it is on screen, and a mark written
   then would come a frame too late. Managed windows have no such problem (below).

The mark is **kiwm's property** (see `kiwm/PROTOCOL.md`), not the compositor's,
because it means something with no compositor at all: a recorder in that case
paints black rectangles over the windows that carry it, which is what kishot does
today. Everything about the compositor in this document is an improvement on
that, and the property is the same either way.

Where the mark comes from:

- **kiwm's window menu** (`kiwm/menu.c`) has a checkable entry, "Hide from screen
  capture", in the manner of Maximize/Restore: it sets `1` on the client or
  deletes it, and (un)marks the window's dialogs with `2`.
- **`kiwm.conf`**, the list that persists across sessions:

  ```ini
  # WM_CLASS names (instance or class, case-insensitive) kiwm always marks
  capture_exclude_classes = KeePassXC, Signal
  ```

  kiwm marks a matching window **when it handles the `MapRequest`, before it maps
  it**, so the mark is there before the window's first frame; it does the same
  for a dialog that opens later from a marked window. kiconf gets an input for the
  list, next to the other kiwm settings.
- **kishot's "Hide a Window..."** sets `1` on the window the user picks. kishot
  has **no remove button**: its list of hidden windows is read-only, with a note
  that to stop hiding a window the user unticks "Hide from screen capture" in
  that window's title-bar menu. A mark can come from a class rule or be inherited
  by a dialog, and kishot undoing it would only be undone in turn; un-marking
  belongs where marks are managed.
- Any tool may; the property is the interface. Under a window manager other than
  kiwm only manual marks exist: no class list, and no marks on dialogs.

Nobody else keeps a list. The compositor and the recorder hold **no set of
windows of their own**: what is excluded is what carries the mark, plus the popup
rule. The marks live as long as their windows do; the class list is what
survives.

### Whether, and how it looks

The user's choice, in `kicomp.conf`, main section:

```ini
# Offer censored capture to recorders, and what an excluded window looks like
# in the picture they get.
#   off      no: the capture protocol is not offered (the default)
#   black    the window is drawn as a solid black shape
#   #rrggbb  ... as a solid shape of that colour
#   hide     the window is not drawn at all; what is behind it shows, which is
#            what kwin_wayland does for the windows it excludes
capture_censor = off
# At most this many sessions open at once; each costs slots (see the cost contract).
capture_sessions = 2
```

`off` is the default because it is not free, and because a recorder that finds
the protocol absent does something perfectly good instead (black rectangles on
its own copy). kiconf is expected to grow a switch for it, and for the mode, next
to the other compositor settings.

## Discovery

### Is there a compositor?

`XGetSelectionOwner(_NET_WM_CM_S<n>)` for the screen being captured. `None`:
no compositor. The recorder censors the way it always has and stops here.

### Does it offer capture?

**`_KICOMP_CAPTURE`** (`CARDINAL[4]`, format 32) **on the CM window** — the same
window that carries `_KICOMP_EFFECTS`, and for the same reason:

| index | meaning |
|---|---|
| 0 | protocol version, `1` |
| 1 | feature bits (below) |
| 2 | censor mode: `0` hide, `1` fill |
| 3 | fill colour as `0x00RRGGBB`; meaningful when the mode is fill |

Feature bits:

| bit | name | meaning |
|---|---|---|
| 0 | `CENSOR` | the compositor can render censored pictures. Always set in version 1; reserved so a later revision can offer plain capture without it |
| 1 | `DMABUF` | slot pixmaps can be exported with DRI3 `BuffersFromPixmap` (only the EGL renderer can say so) |

**Absence is the whole of the negotiation**, as with `_KICOMP_EFFECTS`: no
compositor, one that does not know this protocol, one built without it and one
with `capture_censor = off` all read the same, and the recorder falls back to
its own censoring. A recorder never has to ask *why*.

Why on the CM window and not on the root: the property dies with the window. A
compositor that crashes leaves no stale "yes" on the root for the next recorder
to trust; the CM window is destroyed with its client, and so is the property.

A recorder selects `PropertyChangeMask|StructureNotifyMask` on the CM window,
so it learns of the property appearing (the user turned it on), changing (the
mode changed), or going away (switched off, or the compositor left —
`DestroyNotify`). A new compositor taking over `_NET_WM_CM_S<n>` is a new CM
window: re-run discovery. In every one of those cases a running recording
switches to its own censoring **from the next frame** (see
[Failure](#failure-and-fallback)).

### The version

A recorder that does not know the major version in `_KICOMP_CAPTURE[0]` treats
the protocol as absent. Later revisions add capabilities as feature bits and
never change the meaning of an existing one.

## The session

The recorder creates a **session window S** — an unmapped `InputOnly` window of
its own — puts a small request on it as properties, then sends `OPEN`. The
compositor answers on the same window. Because S belongs to the recorder:

- if the recorder dies, S dies with its connection, the compositor sees
  `DestroyNotify` (it selects `StructureNotify` on S) and frees everything. A
  crashed recorder costs nothing afterwards.
- nothing about the session lives on the CM window, so two recorders do not see
  each other.

### Set by the recorder, on S, before `OPEN`

| property | type | meaning |
|---|---|---|
| `_KICOMP_CAPTURE_CLIENT` | `UTF8_STRING` | the program's name, for logs and the `stats` panel |
| `_KICOMP_CAPTURE_OUTPUTS` | `UTF8_STRING` | RandR names, NUL-separated (`"DP-1\0HDMI-1\0"`). The position in this list is the **session output index** used by every record and message below |
| `_KICOMP_CAPTURE_SLOTS` | `CARDINAL` | slots per output, 1–4; default 3. A single screenshot asks for 1 |
| `_KICOMP_CAPTURE_HIDE_PID` | `CARDINAL` | optional: a `_NET_WM_PID` whose windows are left out of the picture altogether — the recorder's own, so it need not hide its window and wait for the screen to repaint before capturing |

That is all. The recorder sends **no list of windows**: what is excluded is the
compositor's knowledge, the same for every recorder. The one exception is
`HIDE_PID`, which is not about privacy: windows of that process are **never
drawn** in this session's pictures (no contents, no shadow), whatever
`capture_censor` says, and count in `visible` like any excluded window.

### Messages

All are `ClientMessage`, format 32, sent with `XSendEvent` and an **empty event
mask**, which delivers to the client that created the destination window and
needs no selection — the way `_KICOMP_QUIT` already reaches the compositor.
Messages to the compositor go to the **CM window**; messages to the recorder go
to **S**. `data.l[0]` of every message to the compositor is S.

| message | to | `l[0]` | `l[1]` | `l[2]` | `l[3]` | `l[4]` |
|---|---|---|---|---|---|---|
| `_KICOMP_CAPTURE_OPEN` | compositor | S | version (`1`) | | | |
| `_KICOMP_CAPTURE_CLOSE` | compositor | S | | | | |
| `_KICOMP_CAPTURE_REQUEST` | compositor | S | output | request id | | |
| `_KICOMP_CAPTURE_RELEASE` | compositor | S | output | slot | | |
| `_KICOMP_CAPTURE_STATUS` | recorder | state | error | outputs | version | |
| `_KICOMP_CAPTURE_FRAME` | recorder | output | slot | request id | flags | |

### Answered by the compositor, on S

Written **before** `STATUS` is sent. Both ride one connection, so the recorder
that receives `STATUS` finds the properties already there.

**`_KICOMP_CAPTURE_TARGETS`** (`CARDINAL[]`, format 32): one 13-word record per
session output, in order.

| word | meaning |
|---|---|
| 0 | flags: bit 0 `AVAILABLE`, bit 1 `DMABUF` (this output's slots can be exported) |
| 1, 2 | `x`, `y` of the output in root coordinates, as `int32` |
| 3, 4 | `w`, `h`: its size in root pixels (the *logical* size, the one areas and the pointer are in) |
| 5, 6 | `pw`, `ph`: the size of the slots in pixels: **the output's final size**, what it actually scans out. On a HiDPI output this is larger than `w`, `h`; scale an area by `pw/w`, `ph/h` |
| 7 | depth of the slot pixmaps, `24` |
| 8 | slot count `n` |
| 9–12 | the slot pixmaps' XIDs; unused words are `0` |

The picture is **always at the output's final resolution**: the compositor never
downscales it to the logical size. On a 1× output `pw`×`ph` equals `w`×`h`.

**`STATUS`**: `state` `1` the session is active; `0` it was refused, or has
ended (the recorder forgets it). `error`:

| error | meaning |
|---|---|
| 0 | none |
| 1 | unsupported protocol version |
| 2 | disabled (`capture_censor = off`) |
| 3 | an output name is unknown |
| 4 | out of resources: too many sessions, or the slots could not be allocated |
| 5 | not allowed (the server refused the compositor something) |

A refused session is the same as absent capture to the recorder: its own
black rectangles.

### Ending

The recorder sends `CLOSE` (or simply dies). The compositor frees the slots and
**deletes the session properties it wrote** *before* freeing the pixmaps, for
the reason `_KICOMP_STOWED_PIXMAP` gives: a reader that re-reads the property
per use can never be holding an XID the compositor has already freed. The
compositor ending a session itself (it is exiting, or `capture_censor` was
switched off) does the same and sends `STATUS` with state `0`.

## What "excluded" means in the picture

### How it is drawn

The picture is the output's scene **assembled again with those windows
substituted**, in the same z-order, which is what makes the old special cases go
away: a window above an excluded one covers it as it does on screen; one below
is not affected; a translucent window above one shows what the exclusion left.

| mode | the excluded window is |
|---|---|
| fill (`black`, `#rrggbb`) | drawn with a solid colour instead of its contents: same shape (rounded corners, shape extension), same geometry, transform, opacity, clip and effect as on screen. Its shadow is drawn as usual — a shadow says nothing about the contents |
| hide | not drawn: no contents, no shadow, no effect that animates it. What was behind it shows |

In both, **every place the compositor draws the window's contents** is
substituted, not only the main one: an expo cell, a cube face (the ones turned
away included), a cover-switch card, a magic-lamp or minimize animation, a
wobbly mesh, a kept picture (`_KICOMP_STOWED_PIXMAP` content drawn as a
thumbnail), a close animation of a window that is already gone
(`window_retain`). The test is one sentence: **no pixel of the picture is
derived from an excluded window's contents.**

What the picture does not contain: the pointer (the recorder composes it, as it
does today). Nothing else is removed — kiwm's own layers (the alt-tab display,
the move/resize wireframe) are ordinary windows in the scene and are in it.

### Damage

A window being excluded is not damage as far as the normal frame goes: when a
mark is set or removed, or an excluded window maps, unmaps or leaves an output,
the region it covered shows the same pixels on screen and different ones in the
picture. So **any change of a window's excluded state invalidates the picture of
the outputs it touches**: the affected region is redrawn at least, and `UNCHANGED`
is never answered across such a change. (KWin forces a full repaint of its
filtered view for the same reason.)

### When it cannot be done

An output for which the compositor cannot render a censored picture says so in
`STATE` (`UNAVAILABLE`, below) and the recorder censors that output itself. The
cases:

- the output is **unredirected** (a fullscreen window drawn straight to the
  screen, `unredirect.c`): the screen is that window and nothing else, so there
  is nothing to composite and the compositor **keeps the output unredirected**
  (kicomp 0.3.70; an earlier draft had it re-composite, which cost a game its
  direct scan-out for the sake of a picture that is one colour). Whatever is
  behind the window is not in the picture, marked or not: `visible` is `0` if
  the fullscreen window is not excluded (the recorder copies the screen), and
  `1` if it is, in which case the picture is **flat** — the fill colour, or
  black in `hide` mode, which has no "what is behind it" to show;
- the renderer fell back or cannot draw an offscreen target for the session;
- the output went away (hot-unplug).

## Which buffer to read

**`_KICOMP_CAPTURE_STATE`** (`CARDINAL[]`, format 32, on S): three words per
session output, in order, kept current by the compositor.

| word | meaning |
|---|---|
| 0 | `epoch`: increases every time the next word changes from 0 to non-0 or back |
| 1 | `visible`: how many excluded windows contribute pixels to this output's picture right now. `0` means the screen **is** the censored picture |
| 2 | flags: bit 0 `UNAVAILABLE` |

The recorder's decision, per output and per frame:

| `STATE` | read | cost to the compositor |
|---|---|---|
| `UNAVAILABLE` | the output, then censor it yourself (black rectangles, as without a compositor) | none |
| `visible` = 0 | the output, exactly as without a compositor; then re-read `STATE` and **discard the frame if `epoch` changed** | none |
| `visible` > 0 | a slot: `REQUEST`, wait for `FRAME`, read it, `RELEASE` | one extra final assembly of the output |

### The ordering rule that makes `visible` = 0 safe

The recorder does not ask the compositor anything when `visible` is 0, so
between its reading `STATE` and its reading the output an excluded window could
appear (the user ticks the menu entry, or an application opens a window that
matches a class rule). The compositor therefore follows **one-sided ordering**:

> **When `visible` goes from 0 to non-0, the compositor writes `STATE` first and
> paints the first frame containing the window after.** Going from non-0 to 0 it
> may write `STATE` late (after the frame that removed the window) — the
> recorder only reads slots a little longer.

The recorder's half is a seqlock:

```
read STATE            -> epoch e, visible 0
copy the output       -> (CopyArea / GetImage of the output's area)
read STATE again      -> epoch e' ; if e' != e, throw the frame away and use REQUEST
```

This is sound because the server processes each connection's requests in
order: the copy is processed before the second read; if the copy saw a frame
with the window in it, that frame was presented after the compositor's
`ChangeProperty`, which was processed first, so the second read sees the new
epoch. A window can appear on the screen for at most one *discarded* frame.

## Getting a frame: `REQUEST`, `FRAME`, `RELEASE`

Pull, not push: the compositor assembles a censored picture only when the
recorder asks, so a recorder running at 30 fps on a 144 Hz output costs 30 extra
assemblies a second, not 144.

`REQUEST` (`output`, `request id`): the recorder wants a censored picture of
that output. Every request gets **exactly one** `FRAME` with the same `request
id`:

- at the output's next repaint, with the picture assembled into a free slot —
  or at once, if the output has not changed since the last picture the
  compositor made for this session, with that same slot and the flag
  `UNCHANGED`;
- or with `slot` = `0xFFFFFFFF` and a failure flag.

`FRAME` flags:

| bit | name | meaning |
|---|---|---|
| 0 | `UNCHANGED` | nothing changed since the last picture for this output; same content, same slot |
| 1 | `BUSY` | no free slot (the recorder holds them all); try again |
| 2 | `UNAVAILABLE` | the output cannot be rendered censored right now; read `STATE` |

### Holding a slot

A slot **delivered** by `FRAME` is **held** until the recorder sends `RELEASE`
for it, and the compositor **does not write into a held slot**. Held is a count:
`UNCHANGED` delivers the same slot again and holds it again, and each delivery
is released by its own `RELEASE`. With `n` slots the recorder can hold at most
`n - 1`; the one left is what the compositor renders into.

The compositor picks the least recently delivered free slot, so a slot is as old
as it can be when it is reused — the longer the gap, the less the GPU work that
read it can still be running.

### What the recorder does with a slot

A slot is an ordinary pixmap (`depth` 24) of the **compositor's** — draw from
it, never free it, never draw into it; an `XFreePixmap` from another client
destroys it for everybody. The same rules as `_KICOMP_STOWED_PIXMAP`: read the
properties per use rather than caching an XID; tolerate the error anyway (a
permissive error handler over the read is all the handling it needs); and what
you get is the picture as of the `FRAME`.

The baseline is the recorder's existing GPU copy with a different source:

```
xcb_copy_area(slot -> my frame pixmap, at the area's offset)   // per output, stitching
... pointer, as before ...
round trip (GetInputFocus)
RELEASE
```

kishot already copies the root into a ring of pixmaps of its own, so a censored
output costs it nothing it was not doing; a multi-output area takes one
`CopyArea` per output into the right place. Slots are in the output's final
pixels (`pw`×`ph`), so on a HiDPI output the recorder scales its crop by
`pw/w`. A consumer that samples the output as a GL texture (the OBS fork) binds
the slot pixmap the way it binds the root's, when `visible` > 0.

**Zero-copy from the slot** (`DMABUF`): with the feature bit and the record
flag, the recorder may export a slot with DRI3 `BuffersFromPixmap` and import it
directly, skipping its own copy. It gives up composing the pointer and the
multi-output stitch on the GPU, so it is only worth it for one output, no
pointer. The compositor allocates slots **without compression modifiers** (no
DCC), which is what encoders and VPP can import; the recorder passes the
modifier the export returns instead of assuming `DRM_FORMAT_MOD_INVALID`.

`RELEASE` rule: **send it only after a round trip following the last request
that reads the slot**, so the server has at least started the read. When the
read goes through a dma-buf import, the recorder must also not release before
its consumer (the encoder) has finished.

## Synchronisation

- **`FRAME` means complete.** The compositor sends it when the slot's GPU work is
  done — it waits on its own fence, asynchronously (a sync fd polled in its main
  loop), not with a `glFinish` that would stall every other output. A recorder
  may read a slot with or without implicit synchronisation and see a finished
  picture. Explicit fences handed to the recorder are for a later version (they
  cannot ride a `ClientMessage`; they would need DRI3 fences).
- **Write-after-read** (the compositor reusing a slot the server is still copying
  from) is covered by `RELEASE`'s round trip plus the kernel's per-buffer fence,
  plus the least-recently-used choice above. The compositor must not rely on the
  first alone.
- **Ordering on the wire**: properties first, then the message that points at
  them — on the compositor's side for `TARGETS`, `STATE` and `STATUS`.
- **Timestamps**: the recorder stamps a frame when `FRAME` arrives. A picture is
  assembled at the output's repaint and announced as soon as its GPU work ends,
  so that is within a frame of the truth; a recorder that wants better can later
  be given MSC/UST in a property.

## Cost contract

What the compositor must guarantee, so a recorder can turn this on without
thinking about it. These are requirements to implement and then **measure**;
none is a claim about any build.

1. **No session, no cost.** No slots, no code on the frame path beyond one test.
2. **A session with nothing excluded visible costs bookkeeping only**: the
   `STATE` words, kept current from window events, and the slots it was given at
   `OPEN`.
3. **Assembled only when asked, and only if the output changed** since the last
   picture for that session (`UNCHANGED` otherwise).
4. **Only the final assembly is done twice.** Everything that does not depend on
   the excluded windows is done once and shared by both pictures (the table
   below).
5. **Memory is the slots**: `n × pw × ph × 4` bytes per requested output
   (1440×900, three slots, about 15 MB), allocated at `OPEN` and freed at
   `CLOSE`; a screenshot session asks for one slot. `capture_sessions` bounds
   the total.

### What is shared and what is duplicated

| work | done |
|---|---|
| window textures, their dma-bufs, damage, the window mirror | once |
| geometry, animation and effect state, the scene graph | once |
| shadows and any intermediate that does not read an excluded window | once |
| the final assembly: walking the scene and drawing the output | **twice**: once as shown, once substituted |
| an intermediate that *does* read an excluded window (a blur of what is behind a translucent window sitting over one) | per picture, only where it differs |

So the worst case is one more final assembly of an output, and only on outputs
where an excluded window is showing and only while a recorder is running.
Anything that makes it more (an effect that recomputes per picture) is a bug in
that effect.

**Recommended way to do the second assembly cheaply:** let the real frame finish;
copy it into the slot (a GPU blit); then redraw into the slot **only the regions
the excluded windows touch** — the window, in `hide` mode its shadow too — with
the substitution, drawing there everything the scene has in those regions, so
what stands above an excluded window still stands above it. The cost then grows
with the *area* of the excluded windows, not with the output. A compositor may
instead assemble the whole output a second time; the contract is the picture,
not the method.

Either way, a slot that is reused need not be redone from scratch: the
compositor can keep, per slot, the damage accumulated since that slot was last
brought up to date (a damage journal, buffer age in GL terms) and redo only
that. KWin's screencast streams do exactly this.

**What kicomp does (0.3.69), and what it measured.** The per-slot damage
journal, not the blit: each slot keeps the damage the real frame got since that
slot was last drawn (plus the damage the real frame drops because nobody sees
it — under an opaque window — when something is hidden, and the rectangle of a
window whose exclusion changed), and the second assembly redraws only that, so
what is drawn twice is what changed, as on screen. A `REQUEST` is served in the
same loop iteration it arrives, not at the output's next repaint (waiting for
that repaint and its vblank was 16 of the 22 ms a picture took). On GLX the
slot is bound as a texture (texture-from-pixmap) and drawn into through a
framebuffer object, then `glFlush`: the server's own use of the buffer is
ordered by the kernel's implicit sync. A GLX pixmap *drawable* gives the same
pixels but spends ~6 ms in its front-buffer flush; a read back into system
memory and `PutImage` works everywhere and is the last resort. Each is checked
on first use (a few pixels read from both sides) and the next is used if the
server does not see the drawing. On the real session (AMD Renoir, GLX, 60 fps
recording of a 1920×1080 output with an emulator animating): with an excluded
window showing, kicomp 2.9 → 4.2 % of a core, the X server unchanged, the GPU
23 → 34 %, no frame lost, a picture ready 0.2–0.6 ms after its `REQUEST`.

## Security

- **The compositor is free.** It sees every window by being the compositor
  (Composite redirection is what it is), and nothing in this protocol gives it
  access it does not already have; it does not need to be let in.
- **Everything is an X drawable.** The compositor never moves pixels outside X:
  a slot is a pixmap named by an XID; reading it is an X request by the recorder
  (`CopyArea`, `GetImage`, `BuffersFromPixmap`). The server therefore stays the
  access-control point for who may read the screen, and so does xisguard where it
  mediates those requests. The compositor does not decide who may capture and
  must not grow a second, weaker way to do it.
- **What to check when implementing** (not a design question): that a recorder
  allowed to capture the screen is also allowed to read a slot. Reading the slot
  is meant to be, for policy, the same as reading the root; if xisguard's rules
  distinguish a read of another client's pixmap, the slots need to be recognised
  as screen-equivalent.
- **`STATUS` error 5** exists for the compositor being refused something it needs
  (creating the pixmaps, say): it fails the session cleanly, and the recorder
  falls back.
- **What it does not protect.** The screen itself shows the excluded windows;
  `GetImage` of the root, anything else's screenshot, xispanel thumbnails from
  `_KICOMP_STOWED_PIXMAP` — all still see them. This is a property of one picture
  handed to one recorder. Anyone who wants to hide a window from every capture
  needs the server to do it.
- **Spoofed sessions.** `OPEN` names a window, and any client can send the message
  with any window id. The compositor only ever writes properties on S and sends
  events to it, and the slots it names are pixmaps any client can already use, so
  the worst a forger gets is a nuisance; `capture_sessions` bounds even that.

## Failure and fallback

The recorder's rule is that **it never copies the output unmasked while
something that should be excluded might be visible**. Concretely:

| event | recorder does |
|---|---|
| no compositor, no `_KICOMP_CAPTURE`, session refused | its own black rectangles on its copy of the output — the behaviour it has now |
| `STATE` says `UNAVAILABLE` for an output | the same, for that output |
| `FRAME` takes longer than the timeout (suggest 100 ms) or answers `BUSY` | repeat the previous frame; do not read the output |
| `visible` > 0 and the session or the slot is gone (`BadPixmap`, `DestroyNotify` on the CM window, `STATUS` 0) | from this frame on, censor with its own rectangles; never read the output unmasked |
| a `visible` = 0 read whose second `STATE` read differs | discard it |

The recorder builds its own rectangles from the same rules as the compositor: the
windows that carry the mark, plus the popup rule. A recorder that has fallen back to black rectangles while the compositor policy
is `hide` is not doing what the user asked; it says so in its status line rather
than silently blacking out.

## For the three programs

### kicomp must

- publish `_KICOMP_CAPTURE` on the CM window iff `capture_censor` is not `off`,
  with the mode and colour; keep it current; delete it when switched off;
- keep the set of excluded windows from the window mirror it already has: the
  mark `_KIWM_CAPTURE_EXCLUDE` on client windows (it watches the property) and the
  popup rule (`_NET_WM_PID`, override-redirect), re-evaluated when a window maps
  and when either changes — so a window is excluded from the first frame it is
  ever painted in;
- on `OPEN`: validate, allocate the slots as X pixmaps (the EGL renderer as GBM
  buffers named with `PixmapFromBuffers` — the machinery it already has for
  frames; XRender and GLX with ordinary server pixmaps) **at the output's final
  size**, write `TARGETS` and `STATE`, send `STATUS`;
- keep `STATE` current with the one-sided ordering;
- on `REQUEST`, make the second final assembly into a free slot — a flag on the
  scene node, not a second scene — and send `FRAME` when the GPU is done;
- leave an unredirected output alone (see above) and answer for it as one flat
  colour when the fullscreen window is excluded;
- free everything on `CLOSE`, on `DestroyNotify` of S and on exit.

### kiwm must

- add the checkable window-menu entry "Hide from screen capture", setting or
  deleting `_KIWM_CAPTURE_EXCLUDE` on the client window, and (un)marking its
  dialogs with `2`;
- read `capture_exclude_classes` from `kiwm.conf` and mark matching windows with
  `1` **before mapping them** (on the `MapRequest`), and dialogs of a marked
  window with `2`, likewise before mapping;
- document the property in `kiwm/PROTOCOL.md`. kiwm is otherwise not involved: it
  does not know about sessions or slots.

### kishot must

- run discovery at the start of every capture, and watch the CM window while
  recording;
- **keep no list of windows or classes of its own** (`censor_classes`,
  `censor_popups` and the Hidden Windows settings page go away): its list of
  hidden windows in the main window is the windows currently carrying the mark;
- keep its own censoring (`ks_censor.c`) as the fallback and as the only path
  with no compositor, built from the marks plus the popup rule; drop it from an
  output only while a session is delivering censored pictures;
- make "Hide a Window..." set `_KIWM_CAPTURE_EXCLUDE` to `1`, with **no remove
  button**: the list is read-only and says that to stop hiding a window the user
  unticks "Hide from screen capture" in its title-bar menu;
- while a session is open, send `HIDE_PID` with its own PID and stop hiding its
  own window around a capture (the hide-and-wait-for-a-repaint it does now stays
  as the fallback);
- take the output the way it does today when `visible` is 0 and read a slot when
  it is not, per output, with the seqlock and the release rules above;
- show what is happening — "hidden windows: compositor (hide)", "…: black
  rectangles" — in the status line, taking the mode from `_KICOMP_CAPTURE`;
- use the same path for a screenshot and for the Recording tab's live preview (a
  session with one slot).

### Neither side changes

The X server and every other client.

## Prior art: KWin

KWin (Wayland, Plasma 6.6) has "Exclude from capture". What it does, read from
its sources and merge requests
([!8442](https://invent.kde.org/plasma/kwin/-/merge_requests/8442),
[!9171](https://invent.kde.org/plasma/kwin/-/merge_requests/9171),
[`filteredsceneview.cpp`](https://invent.kde.org/plasma/kwin/-/raw/master/src/plugins/screencast/filteredsceneview.cpp),
[`outputscreencastsource.cpp`](https://invent.kde.org/plasma/kwin/-/raw/master/src/plugins/screencast/outputscreencastsource.cpp)):

- the property is `Window::excludeFromCapture`, set from the title-bar menu, the
  Task Manager menu, a window rule or the scripting API; transient windows are
  marked **recursively**, so menus and dialogs go too;
- every screencast stream owns a **`FilteredSceneView`**: a second paint of the
  scene, with its own layer and damage tracking, in which a predicate
  (`addWindowFilter`) drops excluded windows while the stacking order is built.
  It renders **straight into the consumer's buffer** (the PipeWire dma-buf), at
  the output's final pixel size, with the cursor as a separate view;
- `pidToHide`: the requesting application's windows can be left out, which is how
  Spectacle keeps itself out of its own recording;
- when a window's flag changes the view calls `addDeviceRepaint(Region::infinite())`,
  because the region the window vacates is not in the normal damage;
- the flag was first for screencasts only ("a user who wants a screenshot can
  minimise the window"); screenshots were made to respect it afterwards, for
  consistency and privacy.

Where this protocol agrees: the scene graph is shared and only the final paint is
done twice, with a window predicate in the stacking-order step; transients follow
their parent; the picture is at the final output size; screenshots are covered
from the start. Where it goes further: KWin builds the filtered view for every
stream whether or not any window is excluded, and has only the "not drawn" look;
here nothing is rendered while no excluded window is visible (`visible` = 0), and
`fill` exists next to `hide`. Where KWin does better, and what this document took
from it: hiding the recorder's own windows (`HIDE_PID`), invalidating on a change
of excluded state (Damage), the per-slot damage journal, and a task-manager menu
as a second place to mark (below).

## Open questions

- **Recorder-supplied buffers.** KWin renders into the consumer's own buffer, so
  there is no copy at all; here the recorder copies the slot into its frame
  pixmap, one full-frame GPU copy per frame (for 4K, about 33 MB). A later version
  could let the recorder hand the compositor the pixmaps of its own ring, and the
  compositor render the censored picture straight into them — EGL only, and the
  crop and the multi-output stitch would have to move to the compositor. Slots
  stay the baseline because they work with every renderer.
- **A second place to mark.** KDE also offers the mark in its Task Manager menu.
  An entry in xispanel's tasklist menu would set and remove the same property, and
  is the natural place for the "unmark" kishot does not have.
- **The popup rule is in two programs.** Matching an override-redirect window to a
  marked one by `_NET_WM_PID` happens in the compositor and in kishot's fallback;
  it should be one function in `shared/`, not two that drift.
- **Explicit sync and timestamps**: a later version, as DRI3 fences and a
  property for MSC/UST.
- **A second recorder.** The OBS fork's XSHM source could speak this too, and with
  the mark being kiwm's it needs no list of its own either; the slot model fits
  its copy-and-read-later structure, and its direct GPU copy is untouched whenever
  nothing is excluded.

## Atoms

| atom | on | type | set by |
|---|---|---|---|
| `_KIWM_CAPTURE_EXCLUDE` | a client window | `CARDINAL` | kiwm (menu, `kiwm.conf` classes, dialogs), kishot, any tool |
| `_KICOMP_CAPTURE` | CM window | `CARDINAL[4]` | compositor |
| `_KICOMP_CAPTURE_CLIENT` | S | `UTF8_STRING` | recorder |
| `_KICOMP_CAPTURE_OUTPUTS` | S | `UTF8_STRING` | recorder |
| `_KICOMP_CAPTURE_SLOTS` | S | `CARDINAL` | recorder |
| `_KICOMP_CAPTURE_HIDE_PID` | S | `CARDINAL` | recorder |
| `_KICOMP_CAPTURE_TARGETS` | S | `CARDINAL[]` | compositor |
| `_KICOMP_CAPTURE_STATE` | S | `CARDINAL[]` | compositor |

Messages: `_KICOMP_CAPTURE_OPEN`, `_CLOSE`, `_REQUEST`, `_RELEASE` (recorder →
CM window), `_STATUS`, `_FRAME` (compositor → S).
