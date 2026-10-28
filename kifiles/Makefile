# kifiles - file manager (GTK2 + GIO).
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 gio-2.0 gio-unix-2.0
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS)) -lpthread

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

SRCS := kifiles.c kf_config.c kf_icons.c kf_pane.c kf_crumbs.c kf_places.c kf_usagecell.c kf_tabs.c kf_hover.c kf_typeahead.c kf_dnd.c kf_clip.c kf_ops.c kf_jobview.c kf_thumbs.c kf_props.c ../shared/xis_sound.c ../shared/xis_winstate.c ../shared/xis_safefile.c ../shared/xis_rootexec.c
OBJS := $(SRCS:.c=.o)
HDRS := kf.h ../shared/xis_i18n.h ../shared/xis_winstate.h ../shared/xis_safefile.h ../shared/xis_rootexec.h

all: kifiles

kifiles: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

pot: po/kifiles.pot

po/kifiles.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kifiles \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kifiles.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kifiles mo
	install -Dm755 kifiles $(DESTDIR)$(BINDIR)/kifiles
	sed 's|@BINDIR@|$(BINDIR)|' kifiles.desktop.in > kifiles.desktop
	install -Dm644 kifiles.desktop $(DESTDIR)$(DESKTOPDIR)/kifiles.desktop
	rm -f kifiles.desktop
	install -Dm644 kidesktop-mimeapps.list $(DESTDIR)$(DESKTOPDIR)/kidesktop-mimeapps.list
	install -Dm644 kifiles.svg $(DESTDIR)$(ICONDIR)/kifiles.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kifiles.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kifiles $(DESTDIR)$(DESKTOPDIR)/kifiles.desktop $(DESTDIR)$(DESKTOPDIR)/kidesktop-mimeapps.list $(DESTDIR)$(ICONDIR)/kifiles.svg
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kifiles.mo; \
	done

clean:
	rm -f kifiles kifiles.desktop $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
