CFLAGS ?= -Wall -Wextra -O2
LIBS := $(shell pkg-config --cflags --libs gtk+-2.0) -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

all: kiconf

kiconf: kiconf.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: kiconf
	install -Dm755 kiconf $(DESTDIR)$(BINDIR)/kiconf

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiconf

clean:
	rm -f kiconf

.PHONY: all clean install uninstall
