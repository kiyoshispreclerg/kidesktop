# kisession - KiDesktop session leader / service supervisor
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
XSESSIONSDIR ?= /usr/share/xsessions
LOCALEDIR := $(PREFIX)/share/locale

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"'
# gettext is part of glibc, so -lintl there would just fail to link (no
# separate library to find) -- only add it where it's actually a separate
# lib (musl, *BSD): try linking a trivial program against it and see.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
LDLIBS := $(shell pkg-config --libs x11 2>/dev/null || echo -lX11) $(LIBINTL)

# po/LINGUAS: one language code per line ("pt_BR", "es", ...), '#' comments.
LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

all: kisession

kisession: kisession.c
	$(CC) $(CFLAGS) -o $@ kisession.c $(LDLIBS)

# ---- translations (see po/README.md for the day-to-day workflow) ----------

pot: po/kisession.pot

po/kisession.pot: kisession.c
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=kisession \
		-o $@ kisession.c

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kisession.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kisession mo
	install -Dm755 kisession $(DESTDIR)$(BINDIR)/kisession
	sed 's|@BINDIR@|$(BINDIR)|' kisession.desktop.in > kisession.desktop
	install -Dm644 kisession.desktop $(DESTDIR)$(XSESSIONSDIR)/kisession.desktop
	rm -f kisession.desktop
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kisession.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kisession
	rm -f $(DESTDIR)$(XSESSIONSDIR)/kisession.desktop
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kisession.mo; \
	done

clean:
	rm -f kisession kisession.desktop po/*.mo

.PHONY: all install uninstall clean pot update-po mo
