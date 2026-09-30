# kisession

Session leader and service supervisor for KiDesktop. The display manager
tracks this process, not the window manager, so a WM crash or a deliberate
`some-wm --replace` doesn't end the session.

It owns two things:

- **KiDesktop's own services** -- listed below, started in order and
  restarted with backoff when they die. `kiconf` edits this list in its
  own tab. Deliberately separate from XDG autostart.
- **XDG autostart** -- regular installed apps, started only once the base
  services are actually up and answering (an app with a tray icon that
  starts before `xispanel` owns the StatusNotifierWatcher bus name gets no
  tray icon for the rest of its run).

## Services (start order)

| service | kind | default | notes |
|---|---|---|---|
| dbus | env | on | session bus + activation environment |
| xisguard | oneshot | on | XNOTIFY permissions; exits by itself without the extension |
| kiconfd | supervised | on | theme/cursor/settings daemon; other services wait on it briefly at startup |
| xismenu | supervised | on | application menu registrar (global menu); must be up before any app starts |
| xisback | supervised | on | wallpaper |
| xispanel | supervised | on | panel/taskbar |
| xiskeys | supervised | on | global hotkeys |
| kimemoryd | supervised | on | clipboard history (see `../kimemory/`) |
| kistoryd | supervised | on | desktop activity history (see `../kistory/`) |
| audio | oneshot | on | pipewire/pulseaudio, only if nothing already started one |
| locker | supervised | on | `xss-lock` + `i3lock` screen locking |
| polkit | supervised | on | polkit authentication agent (needed by kiconf's Sistema tab) |
| wm | wm | on | window manager, see `wm =` below |
| kicomp | oneshot | on | compositor; optional, not restarted (so the Alt+Shift+F12 toggle can turn it off for good until next login) |
| autostart | autostart | on | XDG autostart entries, started after everything above |

## Config

`$XDG_CONFIG_HOME/kisession.conf` (fallback `~/.config/kisession.conf`):

```
wm = <command>          # which window manager to launch; empty or not
                        # installed falls back to the first available
                        # built-in candidate (kiwm, ...)

SERVICE <name> <0|1>    # whitespace/tab separated. A service missing
                        # from the file keeps its built-in default.
```

The file is created with every service at its default if absent.

## Signals

`SIGHUP` re-reads the config and applies the difference: newly enabled
services start, newly disabled ones stop, and a changed `wm =` restarts
just the window manager. `SIGTERM`/`SIGINT` shut the session down:
autostart apps first, then services in reverse start order, then the WM.
