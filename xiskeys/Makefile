# xiskeys - global hotkey daemon for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := -lX11

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: xiskeys

xiskeys: xiskeys.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: xiskeys
	install -Dm755 xiskeys $(DESTDIR)$(BINDIR)/xiskeys

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xiskeys

clean:
	rm -f xiskeys

.PHONY: all install uninstall clean
