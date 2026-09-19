# xismenu

The application menu registrar KiDesktop was missing. Version 0.1.2.

## The problem it exists for

Qt/KF5 applications don't export their menubar over DBus unless they believe a
global menu exists, and the way they decide is by looking for
`com.canonical.AppMenu.Registrar` on the session bus. Under Plasma that name is
owned by kded5's appmenu module -- `kf5/kded/appmenu.so`, a kded *plugin*, so
there is no standalone binary to borrow. In a kisession without kded nothing owns
it, nothing exports, and the whole chain downstream goes quiet:

- kiwm's `appmenu` titlebar button never appears (it is shown only for windows
  carrying `_KDE_NET_WM_APPMENU_*`);
- `xisserve --menu` has nothing to pop;
- xisserve's `globalmenu` search plugin and xispanel's `globalmenu` widget find
  nothing.

None of those needed fixing. The missing piece was upstream of all of them, and
this is it: with xismenu running, kate and other Qt applications export their
menus again.

## What it does

Owns the registrar name, keeps a window -> (bus name, menu object path) table
behind the standard interface (`RegisterWindow`, `UnregisterWindow`,
`GetMenuForWindow`, `GetMenus`, plus the two signals), and -- the part everything
downstream actually reads -- writes two X11 properties on each window that
registers:

| property | value |
|---|---|
| `_KDE_NET_WM_APPMENU_SERVICE_NAME` | the registering client's bus name |
| `_KDE_NET_WM_APPMENU_OBJECT_PATH`  | the menu object path it registered |

It never speaks DBusMenu itself. Consumers read those properties and talk to the
application directly -- which is exactly what lets kiwm offer an application-menu
button without becoming a DBus client (see `../kiwm/README.md`'s
`appmenu_command=` and `../xisserve/appmenu.c`).

Registrations are dropped, and the properties removed, when the owning
application's bus name vanishes: nothing calls `UnregisterWindow` on a crash, and
a stale entry pointing at a dead connection is worse than no entry, since a
consumer would wait for a reply that can never come.

Two files: `xismenu.c` (the registrar) and `gmenu.c` (the translator).
libdbus and xcb, linked directly (unlike xispanel/xisserve, which `dlopen`
libdbus so they still run without it -- a registrar with no bus has nothing to
fall back to). One blocking dispatch loop on the bus, no other event source; the
X connection also drives GTK discovery. ~1.7 MB RSS.

## Start order matters

An application builds its menu exporter **only if the registrar already exists
when the application starts**. Anything launched before xismenu never exports at
all, not even later. kisession therefore starts it before the panel and well
before XDG autostart -- it is a `SERVICE` row there, on by default.

## Options

- `-v`, `--verbose` -- log every call, not just registrations.
- `--no-x-props` -- register without writing the window properties. Isolates "do
  applications export at all?" from "can consumers find it?".
- `--hide-app-menubar` -- also own `org.kde.kappmenu`. Several KF5 applications
  read that name's presence as "something else is showing my menubar" and hide
  their own in-window one, so this is **off by default**: turning it on before the
  menu is reachable somewhere (kiwm's appmenu button, the panel widget) leaves an
  application with no menu at all. It is the switch to flip once the global menu
  is genuinely part of the setup.

It refuses to share the name: if another registrar already owns it, xismenu
says **who** (unique name, pid and process name) and exits rather than sit there
pretending to be the menu service while applications register with the other one.
kisession restarts it with backoff, so the log keeps saying what has to go away.

The usual culprit is a **kded5 left over from a Plasma session**. kisession uses
the systemd user instance's bus (`$XDG_RUNTIME_DIR/bus`), and that instance --
with every KDE daemon still running on it -- outlives the X session that started
it. A kisession launched after a Plasma session on the same user inherits kded5
(and its half-working appmenu module: it hands out `_KDE_NET_WM_APPMENU_OBJECT_PATH`
but no `SERVICE_NAME` without the rest of Plasma), plasmashell, gmenudbusmenuproxy
and the rest. Until they are gone -- a fresh login, or `systemctl --user stop` on
the plasma units -- no registrar of ours can take over, whatever the platform
theme. It also logs which bus it is on and as whom at startup, the first thing to
compare with an application that "doesn't register".

Verified against a clean bus (Xvfb + private session bus, xismenu the only
registrar, no KDE process anywhere): fceux, a plain Qt5 app, registers and gets
both properties with **either** `QT_QPA_PLATFORMTHEME=kde` or `gtk3` (kisession's
default) -- libqgtk3 carries Qt's own generic DBus menubar, plasma-integration is
not required for anything.

## GTK: translated, not registered

GTK itself never calls `RegisterWindow`. What GTK3 does natively, and GTK2 does
through **appmenu-gtk-module** (loaded via `gtk-modules=appmenu-gtk-module` in
`~/.gtkrc-2.0` / `settings.ini` -- GIMP 2.10 is the reference case), is export
the menu as a **GMenuModel** (`org.gtk.Menus` + `org.gtk.Actions`) and say so on
the window: `_GTK_UNIQUE_BUS_NAME`, `_GTK_MENUBAR_OBJECT_PATH`, and for native
GTK3 `_GTK_APPLICATION_OBJECT_PATH`/`_GTK_WINDOW_OBJECT_PATH` for the `app.`/`win.`
action groups. That is a different protocol from DBusMenu, the only one any
consumer here speaks. Under Plasma, `gmenudbusmenuproxy` bridges the two.

`gmenu.c` is that bridge. For every window carrying those properties it serves a
`com.canonical.dbusmenu` object of its own at `/MenuBar/<window>` and points the
window's `_KDE_NET_WM_APPMENU_*` at **itself**, so every existing consumer works
with GTK applications unchanged -- verified with GIMP 2.10 under kiwm: 678 items
from 194 menus, `xisserve --menu` opens it, and a `clicked` on Help > About opens
GIMP's About dialog.

- **Discovery is by X11**, not by the bus: the registrar only ever hears from
  appmenu-gtk-module windows, native GTK3 ones tell nobody. So xismenu watches the
  window manager's `_NET_CLIENT_LIST` and each client's `_GTK_*` properties. A
  gmenu-path registration reaching the registrar is used as one more hint to look
  at that window immediately. This needs a WM that maintains `_NET_CLIENT_LIST`
  (kiwm does; so does every EWMH WM); without one, GTK menus aren't found.
- **Requires appmenu-gtk-module actually loaded** -- the GTK2 and GTK3 module
  builds (Debian/Ubuntu: `appmenu-gtk2-module` + `appmenu-gtk3-module`, both
  from the `appmenu-gtk-module` source, GIMP needs the GTK2 one and any
  classic-widget-menu GTK3 app the other) have to be installed *and* actually
  loaded by the application, which GTK does from `GTK_MODULES` and from the
  `gtk-modules=` key in `~/.gtkrc-2.0` (GTK2) / `~/.config/gtk-3.0/settings.ini`
  (GTK3). kisession sets `GTK_MODULES=appmenu-gtk-module` itself
  (`setup_gtk_modules()`, appended to any existing value) precisely so this
  doesn't depend on those dotfiles keeping that token -- something outside
  this codebase was observed silently dropping `appmenu-gtk-module` from both
  files' `gtk-modules=` list between two otherwise identical sessions, which
  looked exactly like "GIMP/GTK3 apps stopped exporting their menu" until
  traced back to the missing module.
- **Built from the application once, then held until something actually
  invalidates it**: a stale-past-2s *root* `GetLayout` (the menu being reopened
  from the top, a fresh look at whatever the app has now), or the app's own
  `Changed` signal (which also emits `LayoutUpdated`). A subtree `GetLayout`
  never triggers a rebuild by itself, however long the menu has been sitting
  open -- rebuilding reassigns every item's id from scratch, so doing that
  underneath a consumer still holding an id from the last build would turn its
  next click into `DBUS_ERROR_INVALID_ARGS` (this is what "submenus stop
  working after the menu's been open a bit" would look like). Building itself
  is `org.gtk.Menus.Start()` for the root group, then for every group a
  `:submenu`/`:section` refers to until closure, one call per round; plus
  `org.gtk.Actions.DescribeAll()` on each action object for enabled/checked
  state. All local IPC -- GIMP's menu builds in a few milliseconds.
- Sections become runs of items with separators between them; submenus become
  items with children; an item whose action doesn't exist is insensitive, as in
  GTK; a boolean action state becomes a checkmark.
- A `clicked` `Event` becomes `org.gtk.Actions.Activate()` on the object the
  action's prefix names (`unity.` → the menubar object, `app.`/`win.` → theirs).
- Not translated yet: icons, radio groups (shown as plain items), accelerators.

## Building

```sh
make            # needs libdbus-1-dev and libxcb1-dev
sudo make install
```

Built and installed by the top-level KiDesktop `Makefile` along with everything
else.
