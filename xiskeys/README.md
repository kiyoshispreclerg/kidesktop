# xiskeys

Global hotkey daemon for KiDesktop. Grabs `XGrabKey()` on the root window
for *stateless* actions only -- anything that's just "run a shell command"
and needs no other daemon's live state (media/volume/brightness keys,
screenshot, lock, power). Hotkeys that need a specific daemon's live state
(kiwm's Alt+Tab/maximize/desktop-grid, xispanel's launcher/menu popups)
stay grabbed locally by that daemon, via that daemon's own config --
xiskeys reads those configs at load time and skips (with a log line)
anything already claimed there, instead of racing for the same grab.

## Config

`$XDG_CONFIG_HOME/xiskeys.conf` (fallback `~/.config/xiskeys.conf`),
tab-separated lines:

```
BIND\t<action-name>\t<key-spec>\t<shell command>
```

`<key-spec>` is `<Mod>+<Mod>+...+<Key>` (Ctrl/Alt/Shift/Meta, X11 keysym
name for `<Key>`), the same grammar `xispanel`'s per-widget `hotkey=`
option uses. `<action-name>` is just a label for logs/config
readability.

If the file doesn't exist, a default one is written covering task
manager, screenshot, media keys, volume, brightness, and power/session
actions (the last routed through `xisserve --session` so a stray tap
always asks before it acts).

`kiconf`'s Atalhos tab edits this file directly and signals `xiskeys` with
`SIGHUP` to reload -- ungrabs everything, re-reads, re-grabs.

## Signals

`SIGHUP` reloads the config.
