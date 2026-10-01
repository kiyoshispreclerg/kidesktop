CFLAGS ?= -Wall -Wextra -O2
# pango/pangocairo: exploratory, branch xispanel-plus-pango only -- see
# pango_text.c's file comment. Pulls GLib into xispanel's own process for
# the first time, a real departure from this project's "no toolkit"
# design elsewhere -- not made build-optional (unlike xcomposite/xtst
# above) because this branch's whole point is deciding whether that
# tradeoff is worth it, not shipping a graceful-degradation path yet.
PKGS := x11 xrandr cairo cairo-xlib imlib2 freetype2 fontconfig pango pangocairo

# dbus-1 (mpris.c/sni.c's MPRIS media controls + system tray, dbusmenu.c's
# shared com.canonical.dbusmenu client used by both sni.c and the
# globalmenu widget) is the one *build-time* optional dependency: if
# `pkg-config dbus-1` isn't found (no libdbus-1-dev installed), we link
# mpris_stub.c/sni_stub.c/dbusmenu_stub.c instead -- harmless no-ops with
# the exact same function signatures, so xispanel.c and the widgets that
# call into them (tasklist.c, widgets/tray.c, widgets/globalmenu.c) never
# need to know or care which trio got built. This is on top of (not
# instead of) mpris.c/sni.c/dbusmenu.c's own runtime dlopen() of
# libdbus-1.so.3: even on a system that *does* have the dev headers, none
# of them ever links libdbus-1 directly, so a system with headers but no
# libdbus-1.so.3 installed still runs fine too. See mpris.c/sni.c/
# dbusmenu.c/mpris_stub.c/sni_stub.c/dbusmenu_stub.c.
HAVE_DBUS := $(shell pkg-config --exists dbus-1 && echo yes)
ifeq ($(HAVE_DBUS),yes)
DBUS_SRCS := mpris.c sni.c dbusmenu.c notifd.c
DBUS_CPPFLAGS := $(shell pkg-config --cflags dbus-1)
else
DBUS_SRCS := mpris_stub.c sni_stub.c dbusmenu_stub.c notifd_stub.c
DBUS_CPPFLAGS :=
$(info xispanel: dbus-1 not found via pkg-config -- building without MPRIS/tray/globalmenu/notifications support)
endif

# xcomposite + xdamage (thumb.c's window-thumbnail capture for tasklist's
# show_thumbs=yes tooltip -- xcomposite to read a window's live composited
# pixmap, xdamage to know exactly when it changed so repaints track real
# content instead of a fixed polling interval) are linked normally,
# unlike dbus-1 -- there's no runtime-dlopen equivalent for an X
# extension the way there is for a shared library, since talking to it
# goes through the already-open X connection. If either -dev package
# isn't installed, thumb_stub.c (always reports "no compositor support")
# is linked instead -- no build-time dependency in that case either. See
# thumb.c/thumb_stub.c.
HAVE_XCOMPOSITE := $(shell pkg-config --exists xcomposite xdamage && echo yes)
ifeq ($(HAVE_XCOMPOSITE),yes)
THUMB_SRCS := thumb.c
THUMB_CPPFLAGS := $(shell pkg-config --cflags xcomposite xdamage)
THUMB_LDLIBS := $(shell pkg-config --libs xcomposite xdamage)
else
THUMB_SRCS := thumb_stub.c
THUMB_CPPFLAGS :=
THUMB_LDLIBS :=
$(info xispanel: xcomposite/xdamage not found via pkg-config -- building without window thumbnails)
endif

# xtst (the X Record extension -- modtap.c's "tap this modifier alone"
# global-hotkey detection, see hotkey.c's bare-modifier hotkey= spec)
# is linked normally like xcomposite/xdamage above, no dlopen equivalent
# since it's an X extension over the already-open connection. Without
# libxtst-dev, modtap_stub.c (always reports unavailable, hotkey.c then
# just logs and ignores a bare-modifier spec) is linked instead.
HAVE_XTST := $(shell pkg-config --exists xtst && echo yes)
ifeq ($(HAVE_XTST),yes)
MODTAP_SRCS := modtap.c
MODTAP_CPPFLAGS := $(shell pkg-config --cflags xtst)
MODTAP_LDLIBS := $(shell pkg-config --libs xtst)
else
MODTAP_SRCS := modtap_stub.c
MODTAP_CPPFLAGS :=
MODTAP_LDLIBS :=
$(info xispanel: xtst not found via pkg-config -- building without bare-modifier ("tap Meta alone") hotkeys)
endif

# xfixes (density.c's X-DENSITY client support -- see TESTS/X-DENSITY.md --
# needs XFixesSelectSelectionInput to track the _X_DENSITY_MANAGER_S<screen>
# manager selection live) is linked normally like xcomposite/xtst above, no
# dlopen equivalent for an X extension. Without libxfixes-dev, density_stub.c
# (X-DENSITY support always reports absent, every entry point a no-op) is
# linked instead.
HAVE_XFIXES := $(shell pkg-config --exists xfixes && echo yes)
ifeq ($(HAVE_XFIXES),yes)
DENSITY_SRCS := density.c
DENSITY_CPPFLAGS := $(shell pkg-config --cflags xfixes)
DENSITY_LDLIBS := $(shell pkg-config --libs xfixes)
else
DENSITY_SRCS := density_stub.c
DENSITY_CPPFLAGS :=
DENSITY_LDLIBS :=
$(info xispanel: xfixes not found via pkg-config -- building without X-DENSITY support)
endif

# xcb/xcb-randr/x11-xcb (inputscale.c's X-INPUT-SCALE client query -- see
# that file's doc comment) are the one place this otherwise Xlib-only
# codebase touches XCB, needed only because no Xlib-side client binding
# exists yet for this project's own experimental server extension --
# hand-rolling one raw extension request is far simpler via XCB's generic
# xcb_send_request() than via Xlib's unexported extension-request
# internals. XGetXCBConnection() (x11-xcb) shares xispanel's existing Xlib
# connection rather than opening a second one. Without all three -dev
# packages, inputscale_stub.c (always reports "no confinement active") is
# linked instead.
HAVE_XINPUTSCALE := $(shell pkg-config --exists xcb xcb-randr x11-xcb && echo yes)
ifeq ($(HAVE_XINPUTSCALE),yes)
INPUTSCALE_SRCS := inputscale.c
INPUTSCALE_CPPFLAGS := $(shell pkg-config --cflags xcb xcb-randr x11-xcb)
INPUTSCALE_LDLIBS := $(shell pkg-config --libs xcb xcb-randr x11-xcb)
else
INPUTSCALE_SRCS := inputscale_stub.c
INPUTSCALE_CPPFLAGS :=
INPUTSCALE_LDLIBS :=
$(info xispanel: xcb/xcb-randr/x11-xcb not found via pkg-config -- building without X-INPUT-SCALE support)
endif

CPPFLAGS := $(shell pkg-config --cflags $(PKGS)) $(DBUS_CPPFLAGS) $(THUMB_CPPFLAGS) $(MODTAP_CPPFLAGS) $(DENSITY_CPPFLAGS) $(INPUTSCALE_CPPFLAGS)
LDLIBS := $(shell pkg-config --libs $(PKGS)) $(THUMB_LDLIBS) $(MODTAP_LDLIBS) $(DENSITY_LDLIBS) $(INPUTSCALE_LDLIBS) -lm -ldl

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
LOCALEDIR := $(PREFIX)/share/locale
CPPFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"'
# gettext is part of glibc, so -lintl there would just fail to link (no
# separate library to find) -- only add it where it's actually a separate
# lib (musl, *BSD): try linking a trivial program against it and see.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
LDLIBS += $(LIBINTL)

# po/LINGUAS: one language code per line ("pt_BR", "es", ...), '#' comments.
LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

# shared/xis_outputs.c: stable EDID-based output identity, also used by
# xisback and kiconfd. shared/xis_desktop_actions.c: jumplist ([Desktop
# Action ...]) parsing and recent-file Exec substitution, also used by
# xisserve.c's launcher (widgets/tasklist.c's right-click menu is the
# caller here). shared/xis_pactl_subscribe.c: the `pactl subscribe` child
# (spawn/respawn/line-parsing) behind audio_events.c, also used by
# xisserve's --audio page for its sticky-default feature.
# shared/xis_icon_cache.c: naming/placing the on-disk shrunk-icon cache,
# also used by xisserve's icon resolver. All compiled
# directly in, same as every other source file here, so each program
# stays independently buildable and installable on its own (no shared
# library to depend on at runtime).
SRCS := xispanel.c ewmh.c menu.c tooltip.c toast.c launchfx.c xdnd.c hotkey.c $(MODTAP_SRCS) asyncmd.c pulse.c audio_events.c network.c storage.c storage_events.c pango_text.c $(DBUS_SRCS) $(THUMB_SRCS) $(DENSITY_SRCS) $(INPUTSCALE_SRCS) widgets/spacer.c widgets/clock.c widgets/tasklist.c widgets/winctl.c widgets/tray.c widgets/launcher.c widgets/volume.c widgets/globalmenu.c widgets/folder.c widgets/xisserve.c widgets/notif.c widgets/pager.c widgets/monitor.c widgets/energy.c widgets/container.c widgets/network.c widgets/storage.c widgets/clipboard.c ../shared/xis_outputs.c ../shared/xis_spawn.c ../shared/xis_desktop_actions.c ../shared/xis_pactl_subscribe.c ../shared/xis_icon_cache.c
OBJS := $(SRCS:.c=.o)

all: xispanel

xispanel: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c xispanel.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# ---- translations (see po/README.md for the day-to-day workflow) ----------

pot: po/xispanel.pot

po/xispanel.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=xispanel \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/xispanel.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: xispanel mo
	install -Dm755 xispanel $(DESTDIR)$(BINDIR)/xispanel
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/xispanel.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xispanel
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/xispanel.mo; \
	done

clean:
	rm -f xispanel $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
