# xis-sound - play a KiDesktop sound-theme event from the command line
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: xis-sound

xis-sound: xis-sound.c ../shared/xis_sound.c ../shared/xis_sound.h
	$(CC) $(CFLAGS) -o $@ xis-sound.c ../shared/xis_sound.c

install: xis-sound
	install -Dm755 xis-sound $(DESTDIR)$(BINDIR)/xis-sound

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xis-sound

clean:
	rm -f xis-sound

.PHONY: all install uninstall clean
