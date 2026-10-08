# kiconfd - session settings daemon for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := $(shell pkg-config --cflags --libs x11 xcursor xrandr)

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
LOCALEDIR := $(PREFIX)/share/locale
CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"'
# gettext is part of glibc, so -lintl there would just fail to link (no
# separate library to find) -- only add it where it's actually a separate
# lib (musl, *BSD): try linking a trivial program against it and see.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
LIBS += $(LIBINTL)

# po/LINGUAS: one language code per line ("pt_BR", "es", ...), '#' comments.
LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

# shared/xis_outputs.c: stable EDID-based output identity for the saved
# screen layout (kiconfd-screens.conf), also used by xisback/xispanel --
# compiled directly in, same as kiconfd.c itself, so this stays
# independently buildable and installable on its own (no shared library
# to depend on at runtime). This is also what pulls in the xrandr lib
# above -- kiconfd otherwise only ever drives layout changes through the
# `xrandr` CLI, never linking against libXrandr itself before now.
# shared/xis_clock.c: ki-clock.conf's format (alarms/timers, rung via
# `xisserve --ring`), shared with xisserve; xis_spawn.c starts that popup
# as an independent process, same as every other daemon launching one.
SHARED_SRC := ../shared/xis_outputs.c ../shared/xis_clock.c ../shared/xis_spawn.c

all: kiconfd

kiconfd: kiconfd.c $(SHARED_SRC)
	$(CC) $(CFLAGS) -o $@ kiconfd.c $(SHARED_SRC) $(LIBS)

# ---- translations (see po/README.md for the day-to-day workflow) ----------

pot: po/kiconfd.pot

po/kiconfd.pot: kiconfd.c
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=kiconfd \
		-o $@ kiconfd.c

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kiconfd.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kiconfd mo
	install -Dm755 kiconfd $(DESTDIR)$(BINDIR)/kiconfd
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiconfd.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiconfd
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiconfd.mo; \
	done

clean:
	rm -f kiconfd po/*.mo

.PHONY: all install uninstall clean pot update-po mo
