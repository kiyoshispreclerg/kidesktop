# xiskeys - global hotkey daemon for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := -lX11

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: xiskeys

# shared/xis_spawn.c: the one launch path every KiDesktop daemon that
# starts a user application uses (see its header) -- compiled directly
# in, same as in xispanel/xisserve/xisback, so this program stays
# independently buildable with no shared library to depend on.
SHARED_SRC := ../shared/xis_spawn.c

xiskeys: xiskeys.c $(SHARED_SRC)
	$(CC) $(CFLAGS) -o $@ xiskeys.c $(SHARED_SRC) $(LIBS)

install: xiskeys
	install -Dm755 xiskeys $(DESTDIR)$(BINDIR)/xiskeys

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xiskeys

clean:
	rm -f xiskeys

.PHONY: all install uninstall clean
