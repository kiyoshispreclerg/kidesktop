# kiview - image and document viewer (GTK2) and its plugins.
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 gio-unix-2.0
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS))

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share
LIBEXECDIR ?= $(PREFIX)/libexec
DESKTOPDIR ?= /usr/share/applications
ICONDIR ?= /usr/share/icons/hicolor/scalable/apps
LOCALEDIR := $(DATADIR)/locale
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DDATADIR='"$(DATADIR)"' -DLIBEXECDIR='"$(LIBEXECDIR)"' \
              -DGLIB_DISABLE_DEPRECATION_WARNINGS
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL) -lX11 -lXcomposite -lXdamage -lXrender

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kiview.c kv_window.c kv_plugin.c kv_folder.c plugins/libkvp/kvp_msg.c \
        ../shared/xis_winstate.c ../shared/xis_safefile.c ../shared/xis_density_source.c
OBJS := $(SRCS:.c=.o)
HDRS := kv.h plugins/libkvp/kvp_msg.h ../shared/xis_i18n.h ../shared/xis_winstate.h ../shared/xis_density_source.h

# Plugins: libkvp objects are shared by the bitmap plugins.
LIBKVP_OBJS := plugins/libkvp/kvp_bitmap.o plugins/libkvp/kvp_view.o plugins/libkvp/kvp_msg.o
PLUGIN_CFLAGS := $(shell pkg-config --cflags glib-2.0 cairo x11 gdk-pixbuf-2.0)
PLUGIN_LIBS := $(shell pkg-config --libs glib-2.0 cairo x11 gdk-pixbuf-2.0) -lm
# The image plugin's own decoders are optional: without one of these libraries
# its format goes through gdk-pixbuf (WebP then isn't read at all).
IMG_CFLAGS :=
IMG_LIBS :=
ifneq ($(shell pkg-config --exists libjpeg && echo 1),)
IMG_CFLAGS += -DHAVE_JPEG $(shell pkg-config --cflags libjpeg)
IMG_LIBS += $(shell pkg-config --libs libjpeg)
endif
ifneq ($(shell pkg-config --exists libpng && echo 1),)
IMG_CFLAGS += -DHAVE_PNG $(shell pkg-config --cflags libpng)
IMG_LIBS += $(shell pkg-config --libs libpng)
endif
ifneq ($(shell pkg-config --exists libwebp libwebpdemux && echo 1),)
IMG_CFLAGS += -DHAVE_WEBP $(shell pkg-config --cflags libwebp libwebpdemux)
IMG_LIBS += $(shell pkg-config --libs libwebp libwebpdemux)
endif
# giflib has no .pc file on most distributions
ifneq ($(shell echo '\#include <gif_lib.h>' | $(CC) -E -x c - >/dev/null 2>&1 && echo 1),)
IMG_CFLAGS += -DHAVE_GIF
IMG_LIBS += -lgif
endif
IMAGE_OBJS := plugins/image/main.o plugins/image/exif.o plugins/image/dec_jpeg.o plugins/image/dec_png.o \
              plugins/image/dec_gif.o plugins/image/dec_webp.o plugins/image/dec_pixbuf.o
PLUGINS := kiview-image kiview-test

all: kiview $(PLUGINS)

kiview: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS) -lm

kiview-image: $(IMAGE_OBJS) $(LIBKVP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(PLUGIN_LIBS) $(IMG_LIBS)

$(IMAGE_OBJS): plugins/image/image.h
$(IMAGE_OBJS): PLUGIN_CFLAGS += $(IMG_CFLAGS)

kiview-test: plugins/test/main.o $(LIBKVP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(PLUGIN_LIBS)

plugins/%.o: plugins/%.c plugins/libkvp/kvp.h plugins/libkvp/kvp_msg.h plugins/libkvp/kvp_view.h
	$(CC) $(CFLAGS) $(PLUGIN_CFLAGS) -c -o $@ $<

# The frontend's copy of kvp_msg.o is built with the same flags as the plugins'.
plugins/libkvp/kvp_msg.o: plugins/libkvp/kvp_msg.c plugins/libkvp/kvp_msg.h
	$(CC) $(CFLAGS) $(PLUGIN_CFLAGS) -c -o $@ $<

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

check: plugins/libkvp/kvp_view_test
	./plugins/libkvp/kvp_view_test

plugins/libkvp/kvp_view_test: plugins/libkvp/kvp_view_test.c plugins/libkvp/kvp_view.c plugins/libkvp/kvp_msg.c
	$(CC) $(CFLAGS) $(PLUGIN_CFLAGS) -o $@ $^ $(PLUGIN_LIBS)

# Try it from the build tree, with the test plugin too.
run: all
	KIVIEW_LIBEXEC_DIR=$(CURDIR) \
	KIVIEW_PLUGIN_DIR=$(CURDIR)/plugins/image:$(CURDIR)/plugins/test ./kiview $(FILE)

pot: po/kiview.pot

po/kiview.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kiview \
		-o $@ kiview.c kv_window.c kv_plugin.c kv_folder.c

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kiview.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: all mo
	install -Dm755 kiview $(DESTDIR)$(BINDIR)/kiview
	install -Dm755 kiview-image $(DESTDIR)$(LIBEXECDIR)/kiview/kiview-image
	install -Dm644 plugins/image/image.kvplugin $(DESTDIR)$(DATADIR)/kiview/plugins/image.kvplugin
	mimes=`grep -h '^MimeTypes=' plugins/image/image.kvplugin | cut -d= -f2`; \
	sed -e 's|@BINDIR@|$(BINDIR)|' -e "s|@MIMETYPES@|$$mimes|" kiview.desktop.in > kiview.desktop
	install -Dm644 kiview.desktop $(DESTDIR)$(DESKTOPDIR)/kiview.desktop
	rm -f kiview.desktop
	install -Dm644 kiview.svg $(DESTDIR)$(ICONDIR)/kiview.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiview.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiview $(DESTDIR)$(LIBEXECDIR)/kiview/kiview-image \
		$(DESTDIR)$(DATADIR)/kiview/plugins/image.kvplugin \
		$(DESTDIR)$(DESKTOPDIR)/kiview.desktop $(DESTDIR)$(ICONDIR)/kiview.svg
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kiview.mo; \
	done

clean:
	rm -f kiview $(PLUGINS) kiview.desktop $(OBJS) plugins/*/*.o plugins/libkvp/kvp_view_test po/*.mo

.PHONY: all clean install uninstall pot update-po mo check run
