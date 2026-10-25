# kipaint - paint program (GTK2).
CFLAGS ?= -Wall -Wextra -O2
PKGS := gtk+-2.0 zlib
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS := $(shell pkg-config --libs $(PKGS))

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

SRCS := kipaint.c kp_canvas.c kp_config.c kp_doc.c kp_io.c kp_layer.c kp_paint.c kp_palette.c \
        kp_toolbox.c kp_undo.c kp_ora.c kp_layers_panel.c \
        tools/kp_tool_brush.c tools/kp_tool_eraser.c tools/kp_tool_fill.c tools/kp_tool_picker.c \
        tools/kp_tool_pencil.c ../shared/xis_winstate.c
OBJS := $(SRCS:.c=.o)
HDRS := kp.h ../shared/xis_i18n.h ../shared/xis_winstate.h

all: kipaint

kipaint: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(PKG_LIBS) -lm

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

pot: po/kipaint.pot

po/kipaint.pot: $(SRCS)
	xgettext --from-code=UTF-8 --language=C --keyword=_ --keyword=N_ \
		--add-comments=TRANSLATORS: --sort-by-file --package-name=kipaint \
		-o $@ $(SRCS)

update-po: pot
	for lang in $(LINGUAS); do \
		msgmerge --update --backup=off po/$$lang.po po/kipaint.pot; \
	done

mo: $(MOFILES)

po/%.mo: po/%.po
	msgfmt --check -o $@ $<

install: kipaint mo
	install -Dm755 kipaint $(DESTDIR)$(BINDIR)/kipaint
	sed 's|@BINDIR@|$(BINDIR)|' kipaint.desktop.in > kipaint.desktop
	install -Dm644 kipaint.desktop $(DESTDIR)$(DESKTOPDIR)/kipaint.desktop
	rm -f kipaint.desktop
	install -Dm644 kipaint.svg $(DESTDIR)$(ICONDIR)/kipaint.svg
	if [ -z "$(DESTDIR)" ]; then update-desktop-database $(DESKTOPDIR) 2>/dev/null; gtk-update-icon-cache -f -t $(dir $(patsubst %/,%,$(dir $(patsubst %/,%,$(ICONDIR))))) 2>/dev/null; fi; true
	for lang in $(LINGUAS); do \
		install -Dm644 po/$$lang.mo $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kipaint.mo; \
	done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kipaint $(DESTDIR)$(DESKTOPDIR)/kipaint.desktop $(DESTDIR)$(ICONDIR)/kipaint.svg
	for lang in $(LINGUAS); do \
		rm -f $(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/kipaint.mo; \
	done

clean:
	rm -f kipaint kipaint.desktop $(OBJS) po/*.mo

.PHONY: all clean install uninstall pot update-po mo
