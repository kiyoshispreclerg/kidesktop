CFLAGS ?= -Wall -Wextra -O2

PKGS := xcb xcb-randr xcb-shape xcb-sync xcb-xinput xcb-xfixes xcb-cursor xcb-icccm cairo cairo-xcb imlib2 pango pangocairo

# atk + atk-bridge-2.0: optional, Alt+Tab announced to screen readers
# (a11y.c); a11y_stub.c without them.
HAVE_A11Y := $(shell pkg-config --exists atk atk-bridge-2.0 && echo yes)
ifeq ($(HAVE_A11Y),yes)
A11Y_SRCS := a11y.c
PKGS += atk atk-bridge-2.0
else
A11Y_SRCS := a11y_stub.c
$(info kiwm: atk/atk-bridge-2.0 not found via pkg-config -- building without screen reader support)
endif

CPPFLAGS := $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
LOCALEDIR := $(PREFIX)/share/locale
CPPFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"'
# gettext is part of glibc, so -lintl there would just fail to link (no
# separate library to find) -- only add it where it's actually a separate
# lib (musl, *BSD): try linking a trivial program against it and see.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
LDLIBS += $(LIBINTL)

# po/LINGUAS: one language code per line ("pt_BR", "es", ...), '#' comments.
LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

# ../shared/xis_spawn.c: the one "launch a user application so it is
# genuinely independent of us" path, shared with xispanel/xisserve/
# xisback/xiskeys (see its header) -- compiled straight in, no shared
# library to depend on at runtime. ../shared/xis_direction.c: the shared
# LTR/RTL decision (titlebar order, title/menu alignment, OSD order).
SRCS := main.c config.c atoms.c output.c decoration.c density.c pango_text.c ewmh.c client.c events.c selection.c osd.c tabbox-cover.c menu.c outline.c findcursor.c keybind.c shape.c grip.c sync.c clickraise.c $(A11Y_SRCS) ../shared/xis_spawn.c ../shared/xis_direction.c
OBJS := $(SRCS:.c=.o)
HDRS := $(wildcard *.h)

all: kiwm

kiwm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# ---- translations (see po/README.md for the day-to-day workflow) ----------

pot: po/kiwm.pot

po/kiwm.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=kiwm \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kiwm.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kiwm mo
	install -Dm755 kiwm $(DESTDIR)$(BINDIR)/kiwm
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiwm.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiwm
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiwm.mo; \
	done

clean:
	rm -f kiwm $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
