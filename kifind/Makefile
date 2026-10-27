# kifind - find files (GTK2).
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 gio-2.0
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS))

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share
# Independent of PREFIX like kiconf's: these are the standard freedesktop
# locations app launchers scan, whatever the binary's own prefix.
DESKTOPDIR ?= /usr/share/applications
LOCALEDIR := $(DATADIR)/locale
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DGLIB_DISABLE_DEPRECATION_WARNINGS
# gettext is in glibc; only musl/BSD need a separate -lintl.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL)

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kifind.c
OBJS := $(SRCS:.c=.o)
HDRS := ../shared/xis_i18n.h

all: kifind

kifind: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

pot: po/kifind.pot

po/kifind.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kifind \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kifind.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kifind mo
	install -Dm755 kifind $(DESTDIR)$(BINDIR)/kifind
	sed 's|@BINDIR@|$(BINDIR)|' kifind.desktop.in > kifind.desktop
	install -Dm644 kifind.desktop $(DESTDIR)$(DESKTOPDIR)/kifind.desktop
	rm -f kifind.desktop
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kifind.mo; \
	done

uninstall:
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kifind.mo; \
	done

clean:
	rm -f kifind kifind.desktop $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
