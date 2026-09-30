# kistory

Desktop activity history for KiDesktop. `kistoryd` writes what happened on
the desktop as plain text, one line per event, so it can be searched with
the `kistory` CLI, `grep`, or anything else. It keeps no screenshots and
no window contents, only names: apps, titles, desktops, outputs.

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

A `focus` line is written when the window loses focus, stamped with the
time it gained it, and only if it stayed focused at least `min_dwell_s`.
`app` is the WM_CLASS class (exe basename when there is none); `desktop`
is `*` for sticky windows; `output` comes from kiwm's `_KIWM_WM_OUTPUT`,
else the XRandR monitor under the window.

## Privacy

- `exclude`: apps (WM_CLASS class/instance or exe basename) never logged.
- `title_only_exclude`: apps logged without titles (window/focus lines
  keep app, time and place).
- `private_title_regex`: a POSIX extended regex (case-insensitive); a
  focus period that shows a matching title is logged without titles.
  Empty by default: browsers mark private windows only in the title, in the
  UI language, so there is no universal hint to rely on.
- Pause: while `$XDG_RUNTIME_DIR/kistory-paused.<display>` exists holding
  `0` (indefinitely) or an epoch in the future, nothing is written.
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

## Permissions (xisguard)

In a session guarded by xisguard, kistoryd needs `MANAGE` to read other
clients' window properties, e.g. in `perms.conf`:

```
ALLOW MANAGE /usr/local/bin/kistoryd
```
