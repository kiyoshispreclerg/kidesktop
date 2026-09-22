PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
LOCALEDIR := $(PREFIX)/share/locale
# Independent of PREFIX like kisession's XSESSIONSDIR: these are the
# standard freedesktop locations app launchers scan, regardless of where
# the binary itself gets installed.
DESKTOPDIR ?= /usr/share/applications
ICONDIR ?= /usr/share/icons/hicolor/scalable/apps

CFLAGS ?= -Wall -Wextra -O2
CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"'
# gettext is part of glibc, so -lintl there would just fail to link (no
# separate library to find) -- only add it where it's actually a separate
# lib (musl, *BSD): try linking a trivial program against it and see.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
# x11/xrandr: shared/xis_outputs.c's XRR*/X* calls (make_output_combo()
# in common.c, the Wallpaper/Telas tabs' output picker) -- gtk+-2.0's own
# --libs pulls in gdk-x11-2.0 but not x11/xrandr themselves, and without
# an explicit -lX11 here (only reachable transitively through
# gdk-x11-2.0.so, not from any symbol on this command line directly) the
# linker refuses -lXrandr with "DSO missing from command line".
LIBS := $(shell pkg-config --cflags --libs gtk+-2.0 x11 xrandr) -lm $(LIBINTL)

# shared/xis_outputs.c: stable EDID-based output identity, also used by
# xisback/xispanel/kiconfd -- compiled directly in, same as every other
# source file here, so kiconf stays independently buildable and
# installable on its own (no shared library to depend on at runtime).
SRCS := kiconf.c common.c sysinfo.c tabs/appearance.c tabs/shortcuts.c tabs/entrada.c \
        tabs/outras.c tabs/permissoes.c tabs/paineis.c tabs/wallpaper.c \
        tabs/telas.c tabs/janelas.c tabs/efeitos.c tabs/sistema.c \
        tabs/programas.c tabs/autostart.c tabs/energia.c tabs/eventos.c ../shared/xis_outputs.c

# po/LINGUAS: one language code per line ("pt_BR", "es", ...), '#' comments.
# Empty/missing until the first translation actually exists (see po/README.md).
LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

all: kiconf

kiconf: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

# ---- translations (see po/README.md for the day-to-day workflow) ----------

# Regenerates the message template from every translatable string in SRCS.
# Not part of `all`/`install`: it's a maintainer step, run by hand (or by
# `make update-po`) whenever strings change, then committed like any other
# source file -- translators work from po/kiconf.pot, not from SRCS.
pot: po/kiconf.pot

po/kiconf.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=kiconf \
		-o $@ $(SRCS)

# Re-syncs every po/*.po against a freshly regenerated .pot: new strings
# appear untranslated, removed ones are marked obsolete, everything else
# keeps its translation. Run this after `pot` picks up source changes.
update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kiconf.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kiconf mo
	install -Dm755 kiconf $(DESTDIR)$(BINDIR)/kiconf
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiconf.mo; \
	done
	sed 's|@BINDIR@|$(BINDIR)|' kiconf.desktop.in > kiconf.desktop
	install -Dm644 kiconf.desktop $(DESTDIR)$(DESKTOPDIR)/kiconf.desktop
	rm -f kiconf.desktop
	install -Dm644 kiconf.svg $(DESTDIR)$(ICONDIR)/kiconf.svg
	-update-desktop-database $(DESTDIR)$(DESKTOPDIR) 2>/dev/null
	-gtk-update-icon-cache -f -t $(DESTDIR)/usr/share/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiconf
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiconf.mo; \
	done
	rm -f $(DESTDIR)$(DESKTOPDIR)/kiconf.desktop
	rm -f $(DESTDIR)$(ICONDIR)/kiconf.svg
	-update-desktop-database $(DESTDIR)$(DESKTOPDIR) 2>/dev/null
	-gtk-update-icon-cache -f -t $(DESTDIR)/usr/share/icons/hicolor 2>/dev/null

clean:
	rm -f kiconf po/*.mo kiconf.desktop

.PHONY: all clean install uninstall pot update-po mo
