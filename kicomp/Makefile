CFLAGS ?= -Wall -Wextra -O2

PKGS := xcb xcb-composite xcb-damage xcb-xfixes xcb-render xcb-randr xcb-shape xcb-present

CPPFLAGS := $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

SRCS := src/main.c src/config.c src/output.c src/window.c src/scene.c \
        src/transform.c src/animation.c src/scheduler.c src/effect.c src/shadow.c \
        src/desktop.c src/region.c src/damage.c src/inputscale.c src/density.c \
        src/renderer-xrender.c src/presenter-copy.c src/presenter-present.c \
        src/effects/geometry.c \
        src/effects/fade-in.c src/effects/fade-out.c \
        src/effects/scale-in.c src/effects/scale-out.c \
        src/effects/shade.c src/effects/minimize.c \
        src/effects/desktop-wall.c src/effects/smooth-move.c \
        src/effects/dodge.c
OBJS := $(SRCS:.c=.o)
HDRS := $(wildcard src/*.h)

kicomp: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: kicomp
	install -Dm755 kicomp $(DESTDIR)$(BINDIR)/kicomp

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kicomp

clean:
	rm -f kicomp $(OBJS)

.PHONY: clean install uninstall
