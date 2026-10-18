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
options can be dragged, and its position is kept. Settings opens on the
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
rate, screen copy method and encoder backend; window classes to
always hide.

## How recording works

Capture, cheapest first:

- **GPU copy** -- the X server `CopyArea`s the root window into a pixmap of
  kishot's, which DRI3 exports once as a dma-buf. The libav plugin maps it
  into VAAPI each frame: with a VAAPI encoder the frame never leaves the GPU
  (zero-copy); with a software encoder the GPU converts to NV12 and only that
  is read back. Unlike `GetImage`, nothing stalls the X server on a
  synchronous readback. Censoring and the pointer are drawn server-side on
  the copy (`FillRectangle`, Render `Composite`).
- **SHM** -- MIT-SHM `GetImage` of the root, the classic method.

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

Windows picked with "Hide a Window..." (for this session) or whose WM_CLASS
is listed in the settings are painted black in screenshots and recordings,
optionally with their menus and tooltips (override-redirect windows of the
same `_NET_WM_PID`). Only their visible part is blacked out: opaque managed
windows on top cut it, while shaped and ARGB windows -- which can be seen
through -- don't. The list is kept up to date from `SubstructureNotify` on
the root, never polled per frame.

This happens only in kishot's copy: the screen is unchanged and other capture
programs still see those windows. Showing what is *behind* a hidden window
instead of black would need the compositor (kicomp) to render a second frame
without it; protecting a window from every capture program would be a server
feature (XiS/xisguard), and even there only as black.

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
