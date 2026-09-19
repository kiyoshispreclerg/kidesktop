# kiconfd - session settings daemon for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := $(shell pkg-config --cflags --libs x11 xcursor xrandr)

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

# shared/xis_outputs.c: stable EDID-based output identity for the saved
# screen layout (kiconfd-screens.conf), also used by xisback/xispanel --
# compiled directly in, same as kiconfd.c itself, so this stays
# independently buildable and installable on its own (no shared library
# to depend on at runtime). This is also what pulls in the xrandr lib
# above -- kiconfd otherwise only ever drives layout changes through the
# `xrandr` CLI, never linking against libXrandr itself before now.
SHARED_SRC := ../shared/xis_outputs.c

all: kiconfd

kiconfd: kiconfd.c $(SHARED_SRC)
	$(CC) $(CFLAGS) -o $@ kiconfd.c $(SHARED_SRC) $(LIBS)

install: kiconfd
	install -Dm755 kiconfd $(DESTDIR)$(BINDIR)/kiconfd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiconfd

clean:
	rm -f kiconfd

.PHONY: all install uninstall clean
