# kifiles - file manager (GTK2 + GIO).
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 gio-2.0 gio-unix-2.0
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS)) -lpthread

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share
LOCALEDIR := $(DATADIR)/locale
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DGLIB_DISABLE_DEPRECATION_WARNINGS
# gettext is in glibc; only musl/BSD need a separate -lintl.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL)

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kifiles.c kf_config.c kf_icons.c kf_pane.c kf_crumbs.c kf_places.c kf_dnd.c kf_clip.c kf_ops.c kf_thumbs.c
OBJS := $(SRCS:.c=.o)
HDRS := kf.h ../shared/xis_i18n.h

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
	install -Dm644 kifiles.desktop $(DESTDIR)$(DATADIR)/applications/kifiles.desktop
	rm -f kifiles.desktop
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kifiles.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kifiles $(DESTDIR)$(DATADIR)/applications/kifiles.desktop
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kifiles.mo; \
	done

clean:
	rm -f kifiles kifiles.desktop $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
