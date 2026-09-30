# kistory - desktop activity history for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := -lX11 -lXRes -lXrandr

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kistoryd

# shared/xis_winident.c: window/app/document identity, shared with
# kimemoryd -- compiled directly in, same as the other shared/ sources.
SHARED_SRC := ../shared/xis_winident.c
DAEMON_SRC := kistoryd.c ks_log.c ks_windows.c ks_files.c
DAEMON_HDR := ks_config.h ks_log.h ks_windows.h ks_files.h ../shared/xis_winident.h

kistoryd: $(DAEMON_SRC) $(DAEMON_HDR) $(SHARED_SRC)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(SHARED_SRC) $(LIBS)

install: kistoryd
	install -Dm755 kistoryd $(DESTDIR)$(BINDIR)/kistoryd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kistoryd

clean:
	rm -f kistoryd

.PHONY: all install uninstall clean
