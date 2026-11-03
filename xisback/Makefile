CFLAGS ?= -Wall -Wextra -O2
LIBS := $(shell pkg-config --cflags --libs x11 xrandr xfixes xrender imlib2) -lm

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

# shared/xis_outputs.c: stable EDID-based output identity, also used by
# xispanel and kiconfd -- compiled directly in, same as every other
# source file here, so each program stays independently buildable and
# installable on its own (no shared library to depend on at runtime).
SHARED_SRC := ../shared/xis_outputs.c ../shared/xis_spawn.c ../shared/xis_safefile.c

all: xisback

xisback: xisback.c $(SHARED_SRC)
	$(CC) $(CFLAGS) -o $@ xisback.c $(SHARED_SRC) $(LIBS)

install: xisback
	install -Dm755 xisback $(DESTDIR)$(BINDIR)/xisback

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/xisback

clean:
	rm -f xisback

.PHONY: all clean install uninstall
