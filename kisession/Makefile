# kisession - KiDesktop session launcher (test build)
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
XSESSIONSDIR ?= /usr/share/xsessions

install: kisession
	install -Dm755 kisession $(DESTDIR)$(BINDIR)/kisession
	sed 's|@BINDIR@|$(BINDIR)|' kisession.desktop.in > kisession.desktop
	install -Dm644 kisession.desktop $(DESTDIR)$(XSESSIONSDIR)/kisession.desktop
	rm -f kisession.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kisession
	rm -f $(DESTDIR)$(XSESSIONSDIR)/kisession.desktop

.PHONY: install uninstall
