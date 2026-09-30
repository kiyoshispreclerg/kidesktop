# kimemory - clipboard history for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := -lX11 -lXfixes -lXRes -lXrandr

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kimemoryd

# shared/xis_winident.c: window/app/document identity, shared with
# kistoryd -- compiled directly in, same as the other shared/ sources.
SHARED_SRC := ../shared/xis_winident.c
DAEMON_SRC := kimemoryd.c km_clip.c km_store.c

kimemoryd: $(DAEMON_SRC) km_clip.h km_store.h $(SHARED_SRC) ../shared/xis_winident.h
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(SHARED_SRC) $(LIBS)

install: kimemoryd
	install -Dm755 kimemoryd $(DESTDIR)$(BINDIR)/kimemoryd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kimemoryd

clean:
	rm -f kimemoryd

.PHONY: all install uninstall clean
