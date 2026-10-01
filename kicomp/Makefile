CFLAGS ?= -Wall -Wextra -O2

PKGS := epoxy egl gbm pangocairo x11 x11-xcb xcb xcb-keysyms xcb-composite xcb-damage xcb-xfixes xcb-render xcb-randr xcb-shape xcb-present xcb-dri3 libdrm xcb-xkb

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

SRCS := src/main.c src/config.c src/dri3.c src/output.c src/renderer.c src/window.c src/unredirect.c src/scene.c \
        src/transform.c src/animation.c src/scheduler.c src/effect.c src/shadow.c \
        src/desktop.c src/region.c src/damage.c src/stowpix.c src/inputscale.c src/density.c src/input.c src/text.c \
        src/renderer-xrender.c src/renderer-gl.c src/renderer-glx.c src/renderer-egl.c src/presenter-copy.c src/presenter-present.c src/presenter-glx.c \
        src/effects/geometry.c \
        src/effects/fade-in.c src/effects/fade-out.c \
        src/effects/scale-in.c src/effects/scale-out.c \
        src/effects/slide-in.c src/effects/slide-out.c \
        src/effects/shade.c src/effects/minimize.c \
        src/effects/desktop-wall.c src/effects/smooth-move.c \
        src/effects/dodge.c \
        src/effects/show-windows.c \
        src/effects/cover-switch.c \
        src/effects/cube.c \
        src/effects/expo.c \
        src/effects/magic-lamp.c \
        src/effects/wobbly.c \
        src/effects/visual-bell.c \
        src/effects/zoom.c \
        src/effects/stats.c
OBJS := $(SRCS:.c=.o)
HDRS := $(wildcard src/*.h)

all: kicomp

kicomp: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# ---- translations (see po/README.md for the day-to-day workflow) ----------

pot: po/kicomp.pot

po/kicomp.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file \
		--package-name=kicomp \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kicomp.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kicomp mo
	install -Dm755 kicomp $(DESTDIR)$(BINDIR)/kicomp
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kicomp.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kicomp
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kicomp.mo; \
	done

clean:
	rm -f kicomp $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
