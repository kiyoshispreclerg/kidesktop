# kiterm - tabbed terminal emulator (GTK2 + VTE 0.28).
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 vte
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

SRCS := kiterm.c kt_config.c kt_tab.c kt_search.c kt_prefs.c ../shared/xis_sound.c ../shared/xis_winstate.c ../shared/xis_safefile.c
OBJS := $(SRCS:.c=.o)
HDRS := kiterm.h kt_config.h ../shared/xis_i18n.h ../shared/xis_winstate.h

all: kiterm

# Checked here rather than at parse time so `make clean` works without VTE.
check-deps:
	@pkg-config --exists $(PKGS) || { echo "kiterm: gtk+-2.0 and vte (the GTK2 one, libvte-dev 0.28) are required" >&2; exit 1; }

kiterm: check-deps $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

pot: po/kiterm.pot

po/kiterm.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kiterm \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kiterm.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kiterm mo
	install -Dm755 kiterm $(DESTDIR)$(BINDIR)/kiterm
	sed 's|@BINDIR@|$(BINDIR)|' kiterm.desktop.in > kiterm.desktop
	install -Dm644 kiterm.desktop $(DESTDIR)$(DESKTOPDIR)/kiterm.desktop
	rm -f kiterm.desktop
	install -Dm644 kiterm.svg $(DESTDIR)$(ICONDIR)/kiterm.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiterm.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiterm $(DESTDIR)$(DESKTOPDIR)/kiterm.desktop $(DESTDIR)$(ICONDIR)/kiterm.svg
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiterm.mo; \
	done

clean:
	rm -f kiterm kiterm.desktop $(OBJS) po/*.mo

.PHONY: all check-deps clean install uninstall pot update-po mo
