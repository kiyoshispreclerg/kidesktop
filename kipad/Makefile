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
LIBDIR ?= $(PREFIX)/lib
SYNTAXDIR := $(LIBDIR)/kipad/syntax
INCLUDEDIR := $(PREFIX)/include
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DKIPAD_SYNTAXDIR='"$(SYNTAXDIR)"' -DGLIB_DISABLE_DEPRECATION_WARNINGS
# gettext is in glibc; only musl/BSD need a separate -lintl.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL) -ldl

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kipad.c syntax.c ../shared/xis_winstate.c ../shared/xis_safefile.c ../shared/xis_rootexec.c
OBJS := $(SRCS:.c=.o)
HDRS := syntax.h kipad_syntax.h ../shared/xis_i18n.h ../shared/xis_winstate.h ../shared/xis_safefile.h ../shared/xis_rootexec.h

# Syntax highlighting modules, one .so per family (see kipad_syntax.h).
SYNTAX_MODS := $(patsubst %.c,%.so,$(wildcard syntax/*.c))

all: kipad $(SYNTAX_MODS)

kipad: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

syntax/%.so: syntax/%.c syntax/common.h kipad_syntax.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $<

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

install: kipad $(SYNTAX_MODS) mo
	install -Dm755 kipad $(DESTDIR)$(BINDIR)/kipad
	sed 's|@BINDIR@|$(BINDIR)|' kipad.desktop.in > kipad.desktop
	install -Dm644 kipad.desktop $(DESTDIR)$(DESKTOPDIR)/kipad.desktop
	rm -f kipad.desktop
	install -Dm644 kipad.svg $(DESTDIR)$(ICONDIR)/kipad.svg
	install -Dm644 kipad_syntax.h $(DESTDIR)$(INCLUDEDIR)/kipad/kipad_syntax.h
	for m in $(SYNTAX_MODS); do \
		install -Dm644 $$m $(DESTDIR)$(SYNTAXDIR)/$$(basename $$m); \
	done
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kipad.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kipad $(DESTDIR)$(DESKTOPDIR)/kipad.desktop $(DESTDIR)$(ICONDIR)/kipad.svg
	rm -f $(DESTDIR)$(INCLUDEDIR)/kipad/kipad_syntax.h
	for m in $(SYNTAX_MODS); do rm -f $(DESTDIR)$(SYNTAXDIR)/$$(basename $$m); done
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kipad.mo; \
	done

clean:
	rm -f kipad kipad.desktop $(OBJS) po/*.mo syntax/*.so

.PHONY: all clean install uninstall pot update-po mo
