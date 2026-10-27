# kimountd - automounts the disks marked in kiconf's Discos tab, through udisks2
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
CPPFLAGS += $(shell pkg-config --cflags dbus-1)
LIBS := $(shell pkg-config --libs dbus-1)

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

# shared/xis_safefile.c: crash-safe rewrite of ki-mount.conf, compiled in
# like every other shared/ source.
SHARED_SRC := ../shared/xis_safefile.c

all: kimountd

kimountd: kimountd.c $(SHARED_SRC) ../shared/xis_safefile.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ kimountd.c $(SHARED_SRC) $(LIBS)

install: kimountd
	install -Dm755 kimountd $(DESTDIR)$(BINDIR)/kimountd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kimountd

clean:
	rm -f kimountd

.PHONY: all install uninstall clean
