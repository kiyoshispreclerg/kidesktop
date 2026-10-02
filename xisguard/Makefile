# XisGuard - Permission Guard for XiS
CC ?= gcc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LOCALEDIR := $(PREFIX)/share/locale

CFLAGS ?= -O2 -Wall -Wextra -D_FORTIFY_SOURCE=2 -fstack-protector-strong
CFLAGS += -pthread -DLOCALEDIR='"$(LOCALEDIR)"'
# gettext is part of glibc, so -lintl there would just fail to link (no
# separate library to find) -- only add it where it's actually a separate
# lib (musl, *BSD): try linking a trivial program against it and see.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
# Libraries in LDLIBS, not LDFLAGS: a packager's LDFLAGS (dpkg-buildflags)
# replaces the latter wholesale.
LDLIBS := -pthread -lX11 $(LIBINTL)

# po/LINGUAS: one language code per line ("pt_BR", "es", ...), '#' comments.
# Empty/missing until the first translation actually exists (see po/README.md).
LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

all: xisguard

xisguard: xisguard.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# ---- translations (see po/README.md for the day-to-day workflow) ----------

pot: po/xisguard.pot

po/xisguard.pot: xisguard.c
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=xisguard \
		-o $@ xisguard.c

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/xisguard.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: xisguard mo
	install -D -m 755 xisguard $(DESTDIR)$(BINDIR)/xisguard
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/xisguard.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xisguard
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/xisguard.mo; \
	done

clean:
	rm -f xisguard po/*.mo

.PHONY: all clean install uninstall pot update-po mo
