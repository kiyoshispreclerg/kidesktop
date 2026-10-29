# kishot

Screenshots and screen recording for KiDesktop (GTK2), in the spirit of
Spectacle. Part of **kidesktop-base**.

Launching it takes a screenshot of the area used last time -- full screen,
the monitor under the pointer, the active window, or a rectangular region --
and shows it with Save, Save As, Copy and Open Folder. The options are in
two tabs, Screenshot and Recording, each with its own area; the Recording
tab adds the pointer, system sound and/or microphone, and an optional live
preview of what would be recorded (copied on the X server and scaled down with Render,
so it stays cheap on big screens). The divider between the preview and the
options can be dragged, and its position is kept. The Area list also has one row per connected monitor (kept by RandR name such as `DP-1`, `area_output=` in the config). Settings opens on the
page of the tab shown, and each launch picks the tab of what it asks for:
`kishot` the Screenshot tab, `kishot --record` the Recording one.

## Usage

```
kishot [-f|-m|-a|-r] [-b] [-R]
  -f, --fullscreen     every monitor
  -m, --current        the monitor under the pointer
  -a, --activewindow   the active window
  -r, --region         a rectangle selected with the mouse
  -b, --background     save without showing the window
  -R, --record         start recording the area, or stop the recording
```

An area option also becomes the "last used" one, for screenshots or, with
`-R`, for recordings. There is one kishot per
display: a second `kishot ...` hands its command to the running one (over an
abstract Unix socket) and exits -- that is how `kishot --record` stops a
recording. While recording, the window hides itself and a tray icon (click
to stop) shows the elapsed time. With `-b` no window is ever shown; the
result is announced with `notify-send` when it is installed.

The default hotkeys are in xiskeys (see [xiskeys/README.md](../xiskeys/README.md)):
Print, Shift+Print (region), Meta+Shift+Print (window), Meta+Alt+R (record)
and Meta+Shift+R (record a region).

## Settings

`$XDG_CONFIG_HOME/kishot.conf`, edited from the Settings dialog: folders for
screenshots and videos (`$PICTURES`/`$VIDEOS` follow the XDG folders), image
format (whatever gdk-pixbuf can write: png, jpeg, webp, bmp, tiff), JPEG
quality, file name pattern (strftime), save/copy automatically; container
(mkv, mp4, webm), video codec, quality (0..51, used as CRF or QP), frame
rate, screen copy method and encoder backend. (Windows to hide are not a
setting here: they carry a mark -- see "Hiding windows".)

## How recording works

Capture, cheapest first (GPU copy uses a ring of three buffers and encodes the frame
copied one capture earlier, so the encoder never reads a buffer the server may still be
writing or about to overwrite):

- **GPU copy** -- the X server `CopyArea`s the root window into a pixmap of
  kishot's, which DRI3 exports once as a dma-buf. The libav plugin maps it
  into VAAPI each frame: with a VAAPI encoder the frame never leaves the GPU
  (zero-copy); with a software encoder the GPU converts to NV12 and only that
  is read back. Unlike `GetImage`, nothing stalls the X server on a
  synchronous readback. Censoring and the pointer are drawn server-side on
  the copy (`FillRectangle`, Render `Composite`).
- **SHM** -- MIT-SHM `GetImage` of the root, the classic method.

On an output scaled with X-INPUT-SCALE (the compositor draws a smaller logical
desktop magnified to the physical scanout) the frame is the physical
framebuffer, so the pointer is drawn where and as large as the hardware cursor
shows there: position and size multiplied by physical / confine
(`shared/xis_inputscale.c`), refreshed on `XISConfineNotify` and RandR changes.

With "Automatic", GPU copy is used only when the server sets the
`PresentCapabilityXiSRootCopy` bit (0x80000000) in Present's
`QueryCapabilities` reply for the CRTC under the area -- the same check as
the XSHM source of our OBS fork. Without that guarantee a per-CRTC page flip
leaves the root pixmap stale and the video freezes. Forcing "GPU copy" skips
the check. The dma-buf and the encoder are kept on the X server's GPU (found
through `DRI3Open` and sysfs), which matters on hybrid machines.

Encoding: the plugin `kishot-rec-av.so` (libavcodec/libavformat/libavfilter)
when it loads; otherwise BGRX frames are piped into the `ffmpeg` command, with
sound on a second pipe. VAAPI encoders whose driver gives no packed headers
(Mesa radeonsi before 22) produce no extradata, which mkv/mp4 need: the plugin
takes the parameter sets from the first keyframe and writes the header then.
The `ffmpeg` command can't, so with it "auto" prefers a software encoder.

Sound comes from `libpulse-simple` (also PipeWire, through pipewire-pulse):
the monitor of the default sink and/or the default source, mixed.

`KISHOT_AV_LOG=info|verbose|debug` turns on libav's log;
`KISHOT_PLUGIN=/path/to/kishot-rec-av.so` loads another plugin build.

## Hiding windows

A window is hidden when it carries the mark `_KIWM_CAPTURE_EXCLUDE`: from the
window menu ("Hide from screen capture", in kiwm and in xispanel's tasklist),
from `capture_exclude_classes` in `kiwm.conf`, or from "Hide a Window..." here.
kishot keeps no list of windows or classes of its own: the list in the main
window is the windows carrying the mark, and to stop hiding one the user
unticks the entry in its title-bar menu. (An older `censor_classes=` in
`kishot.conf` is ignored; the same names go in `capture_exclude_classes`.)
Without a compositor's picture the marked windows are painted black in
screenshots and recordings, with their menus and tooltips (override-redirect
windows of the same `_NET_WM_PID`). Only their visible part is blacked out:
opaque managed windows on top cut it, while shaped and ARGB windows -- which
can be seen through -- don't. The list is kept up to date from
`SubstructureNotify` on the root and `PropertyNotify` on the mark, never polled
per frame.

This happens only in kishot's copy: the screen is unchanged and other capture
programs still see those windows. Showing what is *behind* a hidden window
instead of black would need the compositor (kicomp) to render a second frame
without it; protecting a window from every capture program would be a server
feature (XiS/xisguard), and even there only as black.

The black rectangles follow the window's X geometry, so under a compositor
they miss it wherever the compositor draws it elsewhere (cube, expo, magic
lamp, wobbly windows). The planned fix moves the exclusion into the compositor,
as kwin_wayland does: the user marks a window (kiwm's window menu, or "Hide a
Window..." here, which sets a property on it), and kicomp renders a second
picture of the output with those windows black, a colour or not drawn at all,
for kishot to read instead of copying the output. kishot would keep the
rectangles as the fallback. The protocol is specified in
[kicomp/CAPTURE-PROTOCOL.md](../kicomp/CAPTURE-PROTOCOL.md)
([em português](../kicomp/CAPTURE-PROTOCOL.pt_BR.md)); the work is on the
`capture-censor` branch.

**Done so far:** the mark is the list (the window menu entry in kiwm and in
xispanel's tasklist, and "Hide a Window..." here, add and remove it); when the
compositor offers the protocol, kishot opens a session and, per frame, asks it
whether anything hidden is showing under the area (`_KICOMP_CAPTURE_STATE`,
read over the same connection that copies, with the seqlock the protocol
describes). If not, the screen already is the censored picture and no
rectangles are painted. Where a hidden window shows and the compositor can
render the output (today: kicomp's XRender renderer, unscaled outputs), kishot
asks for the censored picture (`REQUEST`), waits up to 150 ms for the slot
(`FRAME`), copies the area out of it and gives it back (`RELEASE`) -- in a
screenshot, in the live preview and in a recording (GPU copy, or the SHM path
through `GetImage`). Everything else -- an output the compositor cannot render,
a scaled output, a slot that did not come in time, a changed epoch, no
compositor -- paints the rectangles as before. With a session open kishot also
sends `HIDE_PID` (its own PID) and, when every output can be rendered censored,
no longer hides its main window around a capture: the compositor leaves it out
of the picture. Not done: the zero-copy dma-buf read of a slot. `KISHOT_DEBUG=1` says on stderr which way a
screenshot went; `kishot/tests/capture-session.sh` tests screenshots and a
recording (SHM path) against a nested X server; the GPU-copy recording path is
compiled but not exercised there.

## Dependencies

Required: GTK2, Xlib (Xext, Xfixes), xcb (shm, randr, xfixes, render).

Optional, each turning on a feature when present:

| What | Build | Runtime | Without it |
|---|---|---|---|
| libav plugin | `libavcodec/libavformat/libavfilter/libavutil` dev files (`WITH_AV=0` skips) | those libraries | records via the `ffmpeg` command |
| ffmpeg command | -- | `ffmpeg` in `PATH` | no recording if the plugin is missing too |
| VAAPI | -- | a VAAPI driver | software encoders, SHM capture |
| Sound | -- | `libpulse-simple.so.0` (dlopen) | silent recordings |
| Notifications | -- | `notify-send` | `-b` results only on stderr |

kishot itself never links FFmpeg or libpulse, so it runs on a machine without
them; the Settings dialog shows which of these were found.

## Building

```sh
make            # kishot, plus plugin/kishot-rec-av.so when libav is found
make install    # PREFIX=/usr/local; the plugin goes to $(LIBDIR)/kishot
```

## Tray indicator and the window while recording

While a recording runs a StatusNotifierItem (`ks_tray.c`, plain GDBus) sits in
the tray; clicking it stops the recording. xispanel's tray is
StatusNotifierItem-only, which is why the old `GtkStatusIcon` (XEmbed) never
showed up there; it is still created when no StatusNotifierWatcher is on the
bus. The window stays open while recording; `hide_on_record=1` (Recording tab,
"Hide this window while recording") hides it, for the case where it would
otherwise be in the video. A region is still picked over a frozen screen.
