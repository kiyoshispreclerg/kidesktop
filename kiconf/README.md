# kiconf

Settings app for KiDesktop, remake of the earlier Python/Qt `xisconf` in
GTK2. Each tab edits a daemon's config file directly and then signals that
daemon to reload -- no central settings-daemon protocol of its own; it
talks to whatever protocol each target already has (a config file, a
Unix-socket control protocol, or a handful of `xrandr`/`xinput`/`xset`/
`xprop` subprocess calls).

Tabs build lazily on first visit, and only one tab's content is kept built
at a time, so opening `kiconf` never pays for daemons that aren't running
and switching tabs doesn't grow the window.

## Tabs

| tab | edits |
|---|---|
| Aparencia | `kiconfd`'s cursor theme/size, color palette, GTK2/3/4 + icon theme, Qt style, fonts |
| Atalhos | `xiskeys.conf`'s `BIND` lines (global hotkeys) |
| Telas | screen layout via `xrandr`, draggable canvas, persisted for `kiconfd` to replay at next login |
| Entrada | pointer/touchpad, key repeat/bell, and XiS keyboard flags |
| Wallpaper | `xisback`'s active wallpaper layers, over its control socket |
| Outras | the remaining XiS flag (DisablePrimarySelection), DPMS/screensaver, virtual desktop count |
| Paineis | `xispanel.conf`'s `PANEL`/`WIDGET`/`THEME` records |
| Permissoes | `xisguard`'s runtime mode and rules, over its control socket |
| Gerenciamento de janelas | `kiwm.conf` |
| Efeitos do compositor | `kicomp.conf` |
| Sistema | hostname/locale/timezone, systemd or plain-tools backend |
| Energia | night-light schedule and other power settings, applied by `kiconfd` |
| Programas padrao | default browser/file manager/editor/e-mail client (`xdg-mime`) |
| Associacoes de arquivos | per-extension default application, wider than Programas Padrao |
| Menu de programas | the `.desktop` entries `xispanel`'s launcher and `xismenu`'s global menu pull from |
| Iniciar automaticamente | `kisession`'s own services plus XDG autostart entries |
| Eventos | `ki-events.conf`, read by `xisserve --calendar` |

## Usage

```sh
kiconf              # opens on the tab-picker home page
kiconf --tab <name> # jumps straight to a tab (e.g. `kiconf --tab Eventos`)
kiconf --list-tabs  # lists valid tab names
```
