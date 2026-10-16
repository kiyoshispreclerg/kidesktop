# kilock - runs i3lock with the options from ~/.config/kilock.conf.
CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kilock

kilock: kilock.c
	$(CC) $(CFLAGS) -o $@ kilock.c

install: kilock
	install -Dm755 kilock $(DESTDIR)$(BINDIR)/kilock

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kilock

clean:
	rm -f kilock

.PHONY: all install uninstall clean
