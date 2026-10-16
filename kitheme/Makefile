# kitheme - KiDesktop's cross-toolkit widget theme (see README.md).
#
#   core/      libkitheme.a: config + Cairo renderer, compiled into everything below
#   gtk2/      libkitheme.so, the GTK2 engine
#   export/    kitheme-export: GTK2 gtkrc + GTK3/GTK4 CSS + PNG assets
#   presets/   oxygen, classic, flat
# `make` also generates build/KiTheme (the oxygen preset), the copy that
# gets installed system-wide so the theme is listed before anyone exports.
CFLAGS ?= -Wall -Wextra -O2
PREFIX ?= /usr/local
DATADIR := $(PREFIX)/share
BINDIR := $(PREFIX)/bin
KITHEME_DATADIR := $(DATADIR)/kitheme
# Independent of PREFIX, like kiconf's DESKTOPDIR: GTK2 only looks for
# themes in its own prefix and ~/.themes, and only loads engines from its
# own libdir.
THEMEDIR ?= /usr/share/themes
GTK2_ENGINEDIR ?= $(shell pkg-config --variable=libdir gtk+-2.0)/gtk-2.0/$(shell pkg-config --variable=gtk_binary_version gtk+-2.0)/engines

CORE_CFLAGS := $(shell pkg-config --cflags cairo) -DKITHEME_DATADIR='"$(KITHEME_DATADIR)"' -fPIC
CORE_SRCS := core/kt_color.c core/kt_config.c core/kt_draw.c core/kt_sample.c
CORE_OBJS := $(CORE_SRCS:.c=.o)
CORE_HDRS := core/kitheme.h

GTK2_CFLAGS := $(shell pkg-config --cflags gtk+-2.0) -DGLIB_DISABLE_DEPRECATION_WARNINGS -fPIC
GTK2_LIBS := $(shell pkg-config --libs gtk+-2.0)

EXPORT_LIBS := $(shell pkg-config --libs cairo x11)

all: core/libkitheme.a gtk2/libkitheme.so export/kitheme-export build/KiTheme/index.theme

core/%.o: core/%.c $(CORE_HDRS)
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

core/libkitheme.a: $(CORE_OBJS)
	rm -f $@
	$(AR) rcs $@ $(CORE_OBJS)

gtk2/libkitheme.so: gtk2/kt_gtk2.c core/libkitheme.a $(CORE_HDRS)
	$(CC) $(CFLAGS) $(GTK2_CFLAGS) -shared -Wl,--no-undefined -o $@ gtk2/kt_gtk2.c core/libkitheme.a $(GTK2_LIBS) -lm

export/kitheme-export: export/kitheme-export.c core/libkitheme.a $(CORE_HDRS)
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -o $@ export/kitheme-export.c core/libkitheme.a $(EXPORT_LIBS) -lm

build/KiTheme/index.theme: export/kitheme-export presets/oxygen.conf
	KITHEME_PRESETS=presets ./export/kitheme-export --preset oxygen --dest build/KiTheme

install: all
	install -Dm755 export/kitheme-export $(DESTDIR)$(BINDIR)/kitheme-export
	install -Dm644 gtk2/libkitheme.so $(DESTDIR)$(GTK2_ENGINEDIR)/libkitheme.so
	for p in presets/*.conf; do install -Dm644 $$p $(DESTDIR)$(KITHEME_DATADIR)/$$p; done
	cd build/KiTheme && find . -type f | while read f; do \
		install -Dm644 "$$f" "$(DESTDIR)$(THEMEDIR)/KiTheme/$$f"; done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kitheme-export $(DESTDIR)$(GTK2_ENGINEDIR)/libkitheme.so
	rm -rf $(DESTDIR)$(KITHEME_DATADIR) $(DESTDIR)$(THEMEDIR)/KiTheme

clean:
	rm -f core/*.o core/libkitheme.a gtk2/libkitheme.so export/kitheme-export
	rm -rf build

.PHONY: all install uninstall clean
