# kitask - process list and system monitor (GTK2). NVML is dlopen()ed at runtime.
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 x11 xres
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS)) -ldl -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share
# Independent of PREFIX like kiconf's: these are the standard freedesktop
# locations app launchers scan, whatever the binary's own prefix.
DESKTOPDIR ?= /usr/share/applications
ICONDIR ?= /usr/share/icons/hicolor/scalable/apps
LOCALEDIR := $(DATADIR)/locale
PKG_CFLAGS += -DLOCALEDIR='"$(LOCALEDIR)"' -DGLIB_DISABLE_DEPRECATION_WARNINGS
# gettext is in glibc; only musl/BSD need a separate -lintl.
LIBINTL := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -o /dev/null -lintl 2>/dev/null && echo -lintl)
PKG_LIBS += $(LIBINTL)

LINGUAS := $(shell test -f po/LINGUAS && grep -v '^\#' po/LINGUAS)
MOFILES := $(foreach lang,$(LINGUAS),po/$(lang).mo)

SRCS := kitask.c tsk_proc.c tsk_sys.c tsk_gpu.c tsk_net.c tsk_x11.c tsk_config.c tsk_graph.c tsk_list.c tsk_actions.c tsk_prefs.c tsk_pie.c ../shared/xis_winstate.c ../shared/xis_safefile.c
OBJS := $(SRCS:.c=.o)
HDRS := tsk.h ../shared/xis_i18n.h ../shared/xis_winstate.h

all: kitask

kitask: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

pot: po/kitask.pot

po/kitask.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kitask \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kitask.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kitask mo
	install -Dm755 kitask $(DESTDIR)$(BINDIR)/kitask
	sed 's|@BINDIR@|$(BINDIR)|' kitask.desktop.in > kitask.desktop
	install -Dm644 kitask.desktop $(DESTDIR)$(DESKTOPDIR)/kitask.desktop
	rm -f kitask.desktop
	install -Dm644 kitask.svg $(DESTDIR)$(ICONDIR)/kitask.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kitask.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kitask $(DESTDIR)$(DESKTOPDIR)/kitask.desktop $(DESTDIR)$(ICONDIR)/kitask.svg
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kitask.mo; \
	done

clean:
	rm -f kitask kitask.desktop $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
