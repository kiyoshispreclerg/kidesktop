# kistory - desktop activity history for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := -lX11 -lXRes -lXrandr

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kistoryd kistory

# shared/xis_winident.c: window/app/document identity, shared with
# kimemoryd; shared/xis_pactl_subscribe.c: the `pactl subscribe` child
# xispanel/xisserve also use -- compiled directly in, same as the other
# shared/ sources.
SHARED_SRC := ../shared/xis_winident.c ../shared/xis_pactl_subscribe.c
DAEMON_SRC := kistoryd.c ks_log.c ks_windows.c ks_files.c ks_audio.c ks_guard.c
DAEMON_HDR := ks_config.h ks_log.h ks_windows.h ks_files.h ks_audio.h ks_guard.h \
              ../shared/xis_winident.h ../shared/xis_pactl_subscribe.h

kistoryd: $(DAEMON_SRC) $(DAEMON_HDR) $(SHARED_SRC)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(SHARED_SRC) $(LIBS)

# The CLI reads the day files directly: no X libraries.
kistory: kistory.c
	$(CC) $(CFLAGS) -o $@ kistory.c

install: kistoryd kistory
	install -Dm755 kistoryd $(DESTDIR)$(BINDIR)/kistoryd
	install -Dm755 kistory $(DESTDIR)$(BINDIR)/kistory

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kistoryd $(DESTDIR)$(BINDIR)/kistory

clean:
	rm -f kistoryd kistory

.PHONY: all install uninstall clean
