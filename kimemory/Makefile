# kimemory - clipboard history for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := -lX11 -lXfixes -lXRes -lXrandr

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kimemoryd kimemory

# shared/xis_winident.c: window/app/document identity, shared with
# kistoryd -- compiled directly in, same as the other shared/ sources.
SHARED_SRC := ../shared/xis_winident.c
DAEMON_SRC := kimemoryd.c km_clip.c km_store.c km_ctl.c

kimemoryd: $(DAEMON_SRC) km_clip.h km_store.h km_ctl.h km_json.h $(SHARED_SRC) ../shared/xis_winident.h
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(SHARED_SRC) $(LIBS)

# The CLI only talks to kimemoryd's socket: no X libraries.
kimemory: kimemory.c km_json.h
	$(CC) $(CFLAGS) -o $@ kimemory.c

install: kimemoryd kimemory
	install -Dm755 kimemoryd $(DESTDIR)$(BINDIR)/kimemoryd
	install -Dm755 kimemory $(DESTDIR)$(BINDIR)/kimemory

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kimemoryd $(DESTDIR)$(BINDIR)/kimemory

clean:
	rm -f kimemoryd kimemory

.PHONY: all install uninstall clean
