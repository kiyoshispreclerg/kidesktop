# kishot - screenshots and screen recording (GTK2).
#
# kishot itself needs only GTK2 and a few xcb libraries. Recording through
# libav is a separate plugin, kishot-rec-av.so, built only when the libav*
# development files are found and loaded at runtime with dlopen(); without
# it kishot pipes frames into the ffmpeg command, and without that too it
# only takes screenshots. Sound comes from libpulse-simple, also dlopen()ed.
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 x11 x11-xcb xext xfixes xcb xcb-shm xcb-randr xcb-xfixes xcb-render xrender xrandr
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS)) -ldl -lpthread -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
PLUGINDIR := $(LIBDIR)/kishot
DATADIR := $(PREFIX)/share
# Independent of PREFIX like kiconf's: these are the standard freedesktop
# locations app launchers scan, whatever the binary's own prefix.
DESKTOPDIR ?= /usr/share/applications
ICONDIR ?= /usr/share/icons/hicolor/scalable/apps
LOCALEDIR := $(DATADIR)/locale
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DKISHOT_PLUGINDIR='"$(PLUGINDIR)"' -DGLIB_DISABLE_DEPRECATION_WARNINGS
# gettext is in glibc; only musl/BSD need a separate -lintl.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL)

# the plugin: WITH_AV=0 skips it even when libav is installed
AV_PKGS := libavcodec libavformat libavfilter libavutil
WITH_AV ?= $(shell pkg-config --exists $(AV_PKGS) && echo 1 || echo 0)
ifeq ($(WITH_AV),1)
PLUGIN := plugin/kishot-rec-av.so
AV_CFLAGS := $(shell pkg-config --cflags $(AV_PKGS))
AV_LIBS := $(shell pkg-config --libs $(AV_PKGS)) -lpthread
endif

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kishot.c ks_config.c ks_grab.c ks_censor.c ks_capture.c ks_region.c ks_rec.c ks_audio.c ks_window.c ks_prefs.c ../shared/xis_winstate.c ../shared/xis_safefile.c
OBJS := $(SRCS:.c=.o)
HDRS := ks.h ks_plugin.h ../shared/xis_i18n.h ../shared/xis_winstate.h

all: kishot $(PLUGIN)

kishot: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

# Deprecation warnings off: the libav API moves every major version and
# this file has to build against FFmpeg 4.4 through 7.
plugin/kishot-rec-av.so: plugin/ks_av.c ks_plugin.h
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden -Wno-deprecated-declarations $(AV_CFLAGS) \
		-o $@ plugin/ks_av.c $(AV_LIBS)

pot: po/kishot.pot

po/kishot.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kishot \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kishot.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: all mo
	install -Dm755 kishot $(DESTDIR)$(BINDIR)/kishot
	if [ -f plugin/kishot-rec-av.so ]; then \
		install -Dm644 plugin/kishot-rec-av.so $(DESTDIR)$(PLUGINDIR)/kishot-rec-av.so; \
	fi
	sed 's|@BINDIR@|$(BINDIR)|' kishot.desktop.in > kishot.desktop
	install -Dm644 kishot.desktop $(DESTDIR)$(DESKTOPDIR)/kishot.desktop
	rm -f kishot.desktop
	install -Dm644 kishot.svg $(DESTDIR)$(ICONDIR)/kishot.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kishot.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kishot $(DESTDIR)$(DESKTOPDIR)/kishot.desktop $(DESTDIR)$(ICONDIR)/kishot.svg
	rm -rf $(DESTDIR)$(PLUGINDIR)
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kishot.mo; \
	done

clean:
	rm -f kishot kishot.desktop $(OBJS) plugin/*.so po/*.mo

.PHONY: all clean install uninstall pot update-po mo
