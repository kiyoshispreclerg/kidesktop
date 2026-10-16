# kilock

Starts `i3lock` with the options chosen in kiconf's **Bloqueio de tela** tab.
i3lock has no configuration file, so they live in `~/.config/kilock.conf`:

| key | meaning |
|---|---|
| `color` | background `rrggbb` (i3lock's own white when unset) |
| `image` | PNG shown instead of the plain color (anything else is ignored) |
| `tiling` | `0`/`1`: tile the image |
| `indicator` | `0`/`1`: the unlock indicator after key presses (default 1) |
| `beep` `ignore_empty` `show_failed` | `0`/`1`: beep on a wrong password, skip empty passwords, show the failed-attempt count |
| `pointer_default` | `0`/`1`: keep the mouse pointer visible |

The session's locker (`xss-lock -- kilock`) and the lock hotkey both run it;
extra arguments go to i3lock (`kilock -n`). A bad config never prevents
locking: the worst case is plain i3lock. The idle time before locking is the
screensaver timeout in the Energia tab.
