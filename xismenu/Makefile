# xismenu - application menu registrar for KiDesktop.
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
PKGS := dbus-1 xcb
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs $(PKGS) 2>/dev/null)

# Unlike xispanel/xisserve -- which dlopen libdbus so they still run on a
# system without it -- this one links it directly: a registrar with no bus
# to register on has nothing to fall back to, so a build without libdbus
# would only produce a binary that exits at startup.
ifeq ($(strip $(PKG_LIBS)),)
$(error xismenu: dbus-1 and xcb are required (install libdbus-1-dev and libxcb1-dev))
endif

all: xismenu

xismenu: xismenu.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ xismenu.c $(PKG_LIBS)

install: xismenu
	install -Dm755 xismenu $(DESTDIR)$(BINDIR)/xismenu

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xismenu

clean:
	rm -f xismenu

.PHONY: all install uninstall clean
