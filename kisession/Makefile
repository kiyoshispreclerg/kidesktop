# kisession - KiDesktop session leader / service supervisor
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
XSESSIONSDIR ?= /usr/share/xsessions

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
LDLIBS := $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)

all: kisession

kisession: kisession.c
	$(CC) $(CFLAGS) -o $@ kisession.c $(LDLIBS)

install: kisession
	install -Dm755 kisession $(DESTDIR)$(BINDIR)/kisession
	sed 's|@BINDIR@|$(BINDIR)|' kisession.desktop.in > kisession.desktop
	install -Dm644 kisession.desktop $(DESTDIR)$(XSESSIONSDIR)/kisession.desktop
	rm -f kisession.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kisession
	rm -f $(DESTDIR)$(XSESSIONSDIR)/kisession.desktop

clean:
	rm -f kisession kisession.desktop

.PHONY: all install uninstall clean
