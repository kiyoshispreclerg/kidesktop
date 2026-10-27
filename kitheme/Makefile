# kitheme - KiDesktop's cross-toolkit widget theme (see README.md).
#
#   core/      libkitheme.a: config + Cairo renderer, compiled into everything below
#   gtk2/      libkitheme.so, the GTK2 engine
#   export/    kitheme-export: GTK2 gtkrc + GTK3/GTK4 CSS + PNG assets
#   qt/        kitheme.so, the QStyle plugin, built for Qt5 and Qt6 when found
#   presets/   oxygen, classic, flat (shape only)
#   colors/    claro, escuro, classico (color schemes)
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
CORE_OBJS := $(CORE_SRCS:.c=.o) core/xis_safefile.o
CORE_HDRS := core/kitheme.h

GTK2_CFLAGS := $(shell pkg-config --cflags gtk+-2.0) -DGLIB_DISABLE_DEPRECATION_WARNINGS -fPIC
GTK2_LIBS := $(shell pkg-config --libs gtk+-2.0)

EXPORT_LIBS := $(shell pkg-config --libs cairo x11)

# Qt: each version is optional, built when its development files are
# there. Qt5 through pkg-config; Qt6 through qmake6 (Debian/Ubuntu ship no
# .pc files for it before 6.4).
CXXFLAGS ?= -Wall -Wextra -O2
QT_CXXFLAGS := -std=c++17 -fPIC $(shell pkg-config --cflags cairo)
HAVE_QT5 := $(shell pkg-config --exists Qt5Widgets 2>/dev/null && echo y)
QT5_CFLAGS := $(if $(HAVE_QT5),$(shell pkg-config --cflags Qt5Widgets))
QT5_LIBS := $(if $(HAVE_QT5),$(shell pkg-config --libs Qt5Widgets))
QT5_MOC := $(if $(HAVE_QT5),$(shell pkg-config --variable=host_bins Qt5Core)/moc)
QT5_PLUGINDIR ?= $(if $(HAVE_QT5),$(shell pkg-config --variable=libdir Qt5Core)/qt5/plugins)
QT6_HEADERS := $(shell qmake6 -query QT_INSTALL_HEADERS 2>/dev/null)
HAVE_QT6 := $(if $(QT6_HEADERS),$(shell test -f $(QT6_HEADERS)/QtWidgets/QStylePlugin && echo y))
QT6_CFLAGS := $(if $(HAVE_QT6),-I$(QT6_HEADERS) -I$(QT6_HEADERS)/QtCore -I$(QT6_HEADERS)/QtGui -I$(QT6_HEADERS)/QtWidgets)
QT6_LIBS := $(if $(HAVE_QT6),-L$(shell qmake6 -query QT_INSTALL_LIBS) -lQt6Widgets -lQt6Gui -lQt6Core)
QT6_MOC := $(if $(HAVE_QT6),$(shell qmake6 -query QT_INSTALL_LIBEXECS)/moc)
QT6_PLUGINDIR ?= $(if $(HAVE_QT6),$(shell qmake6 -query QT_INSTALL_PLUGINS))
QT_TARGETS := $(if $(HAVE_QT5),qt/qt5/kitheme.so) $(if $(HAVE_QT6),qt/qt6/kitheme.so)

all: core/libkitheme.a gtk2/libkitheme.so export/kitheme-export build/KiTheme/index.theme $(QT_TARGETS)

core/%.o: core/%.c $(CORE_HDRS)
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

# shared/xis_safefile.c, built here with -fPIC under its own object name:
# shared/*.o are built by other components without it.
core/xis_safefile.o: ../shared/xis_safefile.c ../shared/xis_safefile.h
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ ../shared/xis_safefile.c

core/libkitheme.a: $(CORE_OBJS)
	rm -f $@
	$(AR) rcs $@ $(CORE_OBJS)

gtk2/libkitheme.so: gtk2/kt_gtk2.c core/libkitheme.a $(CORE_HDRS)
	$(CC) $(CFLAGS) $(GTK2_CFLAGS) -shared -Wl,--no-undefined -o $@ gtk2/kt_gtk2.c core/libkitheme.a $(GTK2_LIBS) -lm

export/kitheme-export: export/kitheme-export.c core/libkitheme.a $(CORE_HDRS)
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -o $@ export/kitheme-export.c core/libkitheme.a $(EXPORT_LIBS) -lm

# moc output goes next to the build, included by kt_qt.cpp as kt_qt.moc.
qt/qt5/kt_qt.moc: qt/kt_qt.cpp
	mkdir -p qt/qt5
	$(QT5_MOC) $(QT5_CFLAGS) -o $@ $<

qt/qt5/kitheme.so: qt/kt_qt.cpp qt/qt5/kt_qt.moc qt/kitheme.json core/libkitheme.a $(CORE_HDRS)
	$(CXX) $(CXXFLAGS) $(QT_CXXFLAGS) $(QT5_CFLAGS) -Iqt -Iqt/qt5 -shared -Wl,--no-undefined -o $@ qt/kt_qt.cpp \
		core/libkitheme.a $(QT5_LIBS) $(shell pkg-config --libs cairo) -lm

qt/qt6/kt_qt.moc: qt/kt_qt.cpp
	mkdir -p qt/qt6
	$(QT6_MOC) $(QT6_CFLAGS) -o $@ $<

qt/qt6/kitheme.so: qt/kt_qt.cpp qt/qt6/kt_qt.moc qt/kitheme.json core/libkitheme.a $(CORE_HDRS)
	$(CXX) $(CXXFLAGS) $(QT_CXXFLAGS) $(QT6_CFLAGS) -Iqt -Iqt/qt6 -shared -Wl,--no-undefined -o $@ qt/kt_qt.cpp \
		core/libkitheme.a $(QT6_LIBS) $(shell pkg-config --libs cairo) -lm

build/KiTheme/index.theme: export/kitheme-export presets/oxygen.conf colors/claro.conf
	KITHEME_PRESETS=presets KITHEME_COLORS=colors ./export/kitheme-export --preset oxygen --dest build/KiTheme

install: all
	install -Dm755 export/kitheme-export $(DESTDIR)$(BINDIR)/kitheme-export
	install -Dm644 gtk2/libkitheme.so $(DESTDIR)$(GTK2_ENGINEDIR)/libkitheme.so
	for p in presets/*.conf colors/*.conf; do install -Dm644 $$p $(DESTDIR)$(KITHEME_DATADIR)/$$p; done
	$(if $(HAVE_QT5),install -Dm644 qt/qt5/kitheme.so $(DESTDIR)$(QT5_PLUGINDIR)/styles/kitheme.so)
	$(if $(HAVE_QT6),install -Dm644 qt/qt6/kitheme.so $(DESTDIR)$(QT6_PLUGINDIR)/styles/kitheme.so)
	cd build/KiTheme && find . -type f | while read f; do \
		install -Dm644 "$$f" "$(DESTDIR)$(THEMEDIR)/KiTheme/$$f"; done

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kitheme-export $(DESTDIR)$(GTK2_ENGINEDIR)/libkitheme.so
	rm -rf $(DESTDIR)$(KITHEME_DATADIR) $(DESTDIR)$(THEMEDIR)/KiTheme
	$(if $(HAVE_QT5),rm -f $(DESTDIR)$(QT5_PLUGINDIR)/styles/kitheme.so)
	$(if $(HAVE_QT6),rm -f $(DESTDIR)$(QT6_PLUGINDIR)/styles/kitheme.so)

clean:
	rm -f core/*.o core/libkitheme.a gtk2/libkitheme.so export/kitheme-export
	rm -rf build qt/qt5 qt/qt6

.PHONY: all install uninstall clean
