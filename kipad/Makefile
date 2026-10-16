# kipad - tabbed text editor (GTK2).
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS))

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share
# Independent of PREFIX like kiconf's: these are the standard freedesktop
# locations app launchers scan, whatever the binary's own prefix.
DESKTOPDIR ?= /usr/share/applications
ICONDIR ?= /usr/share/icons/hicolor/scalable/apps
LOCALEDIR := $(DATADIR)/locale
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DGLIB_DISABLE_DEPRECATION_WARNINGS
# gettext is in glibc; only musl/BSD need a separate -lintl.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL)

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kipad.c
OBJS := $(SRCS:.c=.o)
HDRS := ../shared/xis_i18n.h

all: kipad

kipad: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

pot: po/kipad.pot

po/kipad.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kipad \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kipad.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kipad mo
	install -Dm755 kipad $(DESTDIR)$(BINDIR)/kipad
	sed 's|@BINDIR@|$(BINDIR)|' kipad.desktop.in > kipad.desktop
	install -Dm644 kipad.desktop $(DESTDIR)$(DESKTOPDIR)/kipad.desktop
	rm -f kipad.desktop
	install -Dm644 kipad.svg $(DESTDIR)$(ICONDIR)/kipad.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kipad.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kipad $(DESTDIR)$(DESKTOPDIR)/kipad.desktop $(DESTDIR)$(ICONDIR)/kipad.svg
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kipad.mo; \
	done

clean:
	rm -f kipad kipad.desktop $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
