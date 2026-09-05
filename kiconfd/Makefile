# kiconfd - session settings daemon for KiDesktop
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LIBS := $(shell pkg-config --cflags --libs x11 xcursor)

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kiconfd

kiconfd: kiconfd.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: kiconfd
	install -Dm755 kiconfd $(DESTDIR)$(BINDIR)/kiconfd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiconfd

clean:
	rm -f kiconfd

.PHONY: all install uninstall clean
