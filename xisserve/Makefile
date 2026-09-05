CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 x11
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS)) -ldl

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

# dbus-1 (plugins/globalmenu.c's active-window application-menu search,
# via plugins/dbusmenu.c's com.canonical.dbusmenu client) is the one
# *build-time* optional dependency: if `pkg-config dbus-1` isn't found
# (no libdbus-1-dev installed), plugins/dbusmenu_stub.c is linked
# instead -- a harmless "no menu, ever" no-op with the exact same
# function signatures, so globalmenu.c never needs to know or care which
# one got built. This is on top of (not instead of) dbusmenu.c's own
# runtime dlopen() of libdbus-1.so.3: even on a system that does have the
# dev headers, it never links libdbus-1 directly, so a system with
# headers but no libdbus-1.so.3 installed still runs fine too. Same
# trick, same reasoning as xispanel/Makefile's own HAVE_DBUS block --
# see plugins/dbusmenu.c/dbusmenu_stub.c and xispanel/dbusmenu.c.
HAVE_DBUS := $(shell pkg-config --exists dbus-1 && echo yes)
ifeq ($(HAVE_DBUS),yes)
DBUS_SRC := plugins/dbusmenu.c
DBUS_CPPFLAGS := $(shell pkg-config --cflags dbus-1)
else
DBUS_SRC := plugins/dbusmenu_stub.c
DBUS_CPPFLAGS :=
$(info xisserve: dbus-1 not found via pkg-config -- building without the globalmenu search plugin's application-menu support)
endif

# pages/pulse.c (the --audio page's backend) has no build-time dependency
# of its own: it shells out to `pactl` rather than linking libpulse or
# libpipewire, so there is nothing to detect here and nothing to ifdef.
# `pactl` ships with pulseaudio-utils and is also provided by
# pipewire-pulse, so one code path covers both; if it's missing at
# runtime -- or present with no sound server answering -- the page shows
# an explanatory placeholder instead of failing. Same "shell out, don't
# reimplement" call xispanel/pulse.c already made, for the same reason.
SRCS := xisserve.c appmenu.c plugins/terminal.c plugins/globalmenu.c $(DBUS_SRC) \
        pages/calendar.c pages/audio.c pages/pulse.c
OBJS := $(SRCS:.c=.o)
HDRS := xisserve.h plugins/dbusmenu.h pages/pulse.h

all: xisserve

xisserve: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $(DBUS_CPPFLAGS) -c -o $@ $<

install: xisserve
	install -Dm755 xisserve $(DESTDIR)$(BINDIR)/xisserve

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xisserve

clean:
	rm -f xisserve $(OBJS)

.PHONY: all clean install uninstall
