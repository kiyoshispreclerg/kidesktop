# xismenu

The application menu registrar KiDesktop was missing. Version 0.1.0.

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

libdbus and xcb, linked directly (unlike xispanel/xisserve, which `dlopen`
libdbus so they still run without it -- a registrar with no bus has nothing to
fall back to). One blocking dispatch loop on the bus, no other event source; the
X connection is write-only, two property requests per registration. ~1.7 MB RSS.

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

It refuses to share the name: if kded5 (or another registrar) already owns it,
xismenu says so and exits rather than sit there pretending to be the menu service
while applications register with the other one.

## GTK is not covered

GTK applications never call `RegisterWindow`. They export `GMenuModel` through
`_GTK_MENUBAR_OBJECT_PATH` + `_GTK_UNIQUE_BUS_NAME`, which GTK sets by itself with
no registrar involved -- a different protocol that none of the consumers here
read. So the registrar can do nothing for them, and covering them needs a
*translator*, not a handshake.

The natural home for that is this same daemon: watch windows carrying those
properties, serve a `com.canonical.dbusmenu` object per window on xismenu's own
connection, and point `_KDE_NET_WM_APPMENU_*` at itself. Every existing consumer
would then work with GTK applications unchanged, still speaking only DBusMenu. The
cheap alternative is running Plasma's standalone `gmenudbusmenuproxy`, which is one
line in kisession but a Plasma dependency, and whose coverage measured partial
here.

## Building

```sh
make            # needs libdbus-1-dev and libxcb1-dev
sudo make install
```

Built and installed by the top-level KiDesktop `Makefile` along with everything
else.
