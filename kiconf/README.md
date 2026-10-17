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
| Wallpaper | screens drawn to scale with the wallpaper each one shows (slideshows cycle), per-screen/stretched and same/per-desktop modes applied on click; `xisback`'s raw layers under "Avancado" |
| Paineis | `xispanel.conf`'s `PANEL`/`WIDGET`/`THEME` records; also lists installed xispanel widget modules (`<type>.widget` next to the `.so`, see `xispanel/PROTOCOL.md`'s "Widget modules") |
| Permissoes | `xisguard`'s runtime mode and rules, over its control socket |
| Gerenciamento de janelas | `kiwm.conf` |
| Efeitos do compositor | `kicomp.conf` |
| Sistema | hostname/locale/timezone, systemd or plain-tools backend |
| Energia | night-light schedule and other power settings, applied by `kiconfd` |
| Programas padrao | default browser/file manager/editor/e-mail client (`xdg-mime`) |
| Associacoes de arquivos | per-extension default application, wider than Programas Padrao |
| Menu de programas | the `.desktop` entries `xispanel`'s launcher and `xismenu`'s global menu pull from; per-app extra env vars/args applied only inside the KiDesktop session (see `../kisession/README.md`) |
| Iniciar automaticamente | `kisession`'s own services plus XDG autostart entries |
| Usuarios | local accounts; changes queued and applied as root under one `pkexec` prompt |
| Servicos | status/start/stop/enable/disable on systemd, OpenRC or sysvinit, via `pkexec` |
| Login | display manager (SDDM, LightDM, SLiM): autologin, session, Num Lock, theme; written as root under `pkexec` |
| Sons | `kidesktop-sound.conf` (theme, volume, per-event files), read live by every program playing sounds |
| Eventos | `ki-events.conf`, read by `xisserve --calendar` |

## Plugin tabs

Extra tabs are dropped in `<datadir>/kiconf/tabs/` (`~/.local/share/kiconf/tabs/`
first, then each `XDG_DATA_DIRS` entry, then `$(PREFIX)/share`; the first file
with a given id wins; `KICONF_TABS_DIR` adds a dir at the front for testing).
Each plugin is a `<id>.desktop` file:

```
[Desktop Entry]
Name=Hello
Name[pt_BR]=Ola
Comment=Shown as tooltip, also searched
Icon=help-about                 # icon-theme name or absolute path
X-KiConf-Category=system        # appearance|hardware|programs|system, or any text
X-KiConf-Keywords=example;demo
X-KiConf-Module=hello.so        # in-process: exports GtkWidget *kiconf_tab_build(void)
# or, instead of Module:
Exec=my-settings-page           # separate process, embedded via XEmbed:
                                # "%x" -> socket window id, else "--plug=<id>" is appended
```

Modules are loaded lazily on first visit; `--tab <id>` and `--list-tabs`
include plugins. A working example is in `examples/hello/`.

The home page has a search box (matches title, comment and keywords; Enter
opens the first match) and one section per category. Plugin tabs are not part
of the unsaved-edits tracking built-in tabs get.

## Usage

```sh
kiconf              # opens on the tab-picker home page
kiconf --tab <name> # jumps straight to a tab (e.g. `kiconf --tab Eventos`)
kiconf --list-tabs  # lists valid tab names
```
