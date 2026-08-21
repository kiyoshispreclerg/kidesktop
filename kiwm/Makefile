CFLAGS ?= -Wall -Wextra -O2

PKGS := xcb xcb-randr cairo cairo-xcb imlib2

CPPFLAGS := $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

SRCS := main.c atoms.c output.c decoration.c ewmh.c client.c events.c selection.c
OBJS := $(SRCS:.c=.o)
HDRS := $(wildcard *.h)

kiwm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: kiwm
	install -Dm755 kiwm $(DESTDIR)$(BINDIR)/kiwm

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiwm

clean:
	rm -f kiwm $(OBJS)

.PHONY: clean install uninstall
