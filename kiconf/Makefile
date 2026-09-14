CFLAGS ?= -Wall -Wextra -O2
LIBS := $(shell pkg-config --cflags --libs gtk+-2.0) -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

SRCS := kiconf.c common.c tabs/appearance.c tabs/shortcuts.c tabs/entrada.c \
        tabs/outras.c tabs/permissoes.c tabs/paineis.c tabs/wallpaper.c \
        tabs/telas.c

all: kiconf

kiconf: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

install: kiconf
	install -Dm755 kiconf $(DESTDIR)$(BINDIR)/kiconf

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiconf

clean:
	rm -f kiconf

.PHONY: all clean install uninstall
