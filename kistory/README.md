# kistory

Desktop activity history for KiDesktop. `kistoryd` writes what happened on
the desktop as plain text, one line per event, so it can be searched with
the `kistory` CLI, `grep`, or anything else. It keeps no screenshots and
no window contents, only names: apps, titles, files, desktops, outputs,
sound streams.

## Log format

`$XDG_DATA_HOME/kistory/events/YYYY-MM-DD.tsv` (files 0600, directory
0700), one tab-separated line per event, `\t` `\n` `\\` escaped:

```
ts  kind  app  exe  desktop  output  subject  detail
```

```
2026-09-30T14:42:12  win_open  XTerm  /usr/bin/xterm  0  default  kiyoshi@pc: ~/src  win=0x400034 at_start
2026-09-30T14:42:53  focus     XTerm  /usr/bin/xterm  0  default  14:42:53-14:42:58  shell | arquivo.c - vim | outro.h - vim
2026-09-30T14:42:20  win_close XClock /usr/bin/xclock 0  default                     win=0xa0000a open=7s
```

| kind | subject | detail |
|---|---|---|
| `session` | `start` / `stop` | version |
| `win_open`, `win_close` | window title | `win=0x..`, `at_start`, `open=Ns` |
| `focus` | time range | every distinct title shown while focused, ` \| `-separated |
| `desktop` | `switch` / `at_start` | (desktop/output columns) |
| `output` | `connected` / `disconnected` | |
| `file` | path (or URL) | `src=xbel`, `src=kde` or `src=fd` |
| `audio_play`, `audio_rec` | time range | stream's media name, `pid=N` |
| `guard` | time range | `ACTION REPORT\|REQUEST pid=N` (xisguard) |

A `focus` line is written when the window loses focus, stamped with the
time it gained it, and only if it stayed focused at least `min_dwell_s`.
Period lines (focus, audio, guard) are written when the period ends, so a
day file is in write order, not strictly sorted by `ts`.
`app` is the WM_CLASS class (exe basename when there is none); `desktop`
is `*` for sticky windows; `output` comes from kiwm's `_KIWM_WM_OUTPUT`,
else the XRandR monitor under the window.

## Command line

`kistory` reads the day files directly, whether kistoryd runs or not.
D is `YYYY-MM-DD`, `today`, `yesterday` or `Nd` (N days ago).

```
kistory search TERM... [--since D] [--until D] [--kind K] [--app A] [-n N]
kistory day [D]
kistory at 'D HH:MM'        (or just HH:MM for today)
kistory stats [--since D]
kistory purge (--all | [--app A] [--kind K] [--since D] [--until D])
kistory pause [MINUTES] | resume | status
```

`search` matches every TERM case-insensitively anywhere in a line (last 30
days by default, the last N matches). `at` lists the periods covering that
moment and the instant events within 2 minutes of it:

```
$ kistory at 14:51:09
2026-09-30 14:51:09  focus      XTerm            [d0 default]  14:51:09-14:51:15  arquivo.c - vim | orcamento-2026.ods - LibreOffice | shell
2026-09-30 14:51:09  audio_play LibreWolf         14:51:09-14:51:15  AudioStream pid=12565
```

`purge` deletes whole day files for date-only filters and rewrites them
otherwise, under the same `flock` kistoryd appends with.

## Files

Third-party apps don't report what they open, so three partial sources
are combined:

- `xbel`: new or re-used entries of `recently-used.xbel` (GTK file
  chooser, GIMP, Inkscape, LibreOffice...), with the app that used them.
- `kde`: new `RecentDocuments/*.desktop` entries (KDE apps).
- `fd`: when a window loses focus after at least `min_dwell_s`, the regular
  files under `$HOME` its process holds open (players, viewers, editors
  that keep the file open). Caches, config, databases, fonts, dotfiles are
  skipped; the same app and file are logged at most once an hour.
  `fd_sampling=0` turns it off.

Apps whose titles are hidden get no `file` lines at all. The lists read at
start are only a baseline: nothing is logged for entries already there.
It can't tell open from save or export apart, and apps that read a file
and close it without touching the xbel (plain Qt, most CLI tools) leave no
trace.

## Audio

PulseAudio/PipeWire-pulse streams, followed through one `pactl subscribe`
child (`shared/xis_pactl_subscribe.c`) and `pactl list` on each stream
event. A period runs while a stream is uncorked (a paused video's stream
stays open but corked); periods of 2 s or more are logged, with the app's
`application.name` and binary. Level meters ("Peak detect") are ignored,
and the media name is dropped for apps with hidden titles.

## Screen capture and other guarded actions

kistoryd subscribes to xisguard's event stream (`SUBSCRIBE` on the
existing `xisguard-ctl.<display>.sock`; the connection just stays open, no
extra process) for the actions in `guard_actions`. The X server reports an
action every ~2 s while a program keeps using it, so reports less than
15 s apart are merged into one period per program, action and kind
(`REQUEST`: asked for a permission it didn't have; `REPORT`: used one).
If xisguard isn't running, kistoryd retries every 30 s. xisguard only
accepts executables listed in its `subscribers=` (kistoryd is by default).

## Privacy

- `exclude`: apps (WM_CLASS class/instance or exe basename) never logged.
- `title_only_exclude`: apps logged without titles (window/focus lines
  keep app, time and place).
- `private_title_regex`: a POSIX extended regex (case-insensitive); a
  focus period that shows a matching title is logged without titles.
  Empty by default: browsers mark private windows only in the title, in the
  UI language, so there is no universal hint to rely on.
- Pause: while `$XDG_RUNTIME_DIR/kistory-paused.<display>` exists holding
  `0` (indefinitely) or an epoch in the future, nothing is written
  (`kistory pause`/`resume`).
- `retention_days` old day files are deleted at start and every 6 hours.

## Config

`$XDG_CONFIG_HOME/kistory.conf`, `key=value`, `SIGHUP` reloads:

| key | default |
|---|---|
| `min_dwell_s` | 3 |
| `retention_days` | 90 (0: forever) |
| `exclude` | `keepassxc,KeePassXC` |
| `title_only_exclude` | (empty) |
| `private_title_regex` | (empty) |
| `fd_sampling` | 1 |
| `guard_actions` | `SCREEN,RECORD,INPUT_INJECT,INPUT` (empty: don't subscribe) |

## Permissions (xisguard)

In a session guarded by xisguard, kistoryd needs `MANAGE` to read other
clients' window properties, e.g. in `perms.conf`:

```
ALLOW MANAGE /usr/local/bin/kistoryd
```
