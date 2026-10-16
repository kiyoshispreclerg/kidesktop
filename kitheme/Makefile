# kitheme - KiDesktop's cross-toolkit widget theme (see README.md).
#
#   core/      libkitheme.a: config + Cairo renderer, compiled into everything below
#   presets/   oxygen, classic, flat
CFLAGS ?= -Wall -Wextra -O2
PREFIX ?= /usr/local
DATADIR := $(PREFIX)/share
KITHEME_DATADIR := $(DATADIR)/kitheme
CORE_CFLAGS := $(shell pkg-config --cflags cairo) -DKITHEME_DATADIR='"$(KITHEME_DATADIR)"' -fPIC
CORE_SRCS := core/kt_color.c core/kt_config.c core/kt_draw.c core/kt_sample.c
CORE_OBJS := $(CORE_SRCS:.c=.o)
CORE_HDRS := core/kitheme.h

all: core/libkitheme.a

core/%.o: core/%.c $(CORE_HDRS)
	$(CC) $(CFLAGS) $(CORE_CFLAGS) -c -o $@ $<

core/libkitheme.a: $(CORE_OBJS)
	rm -f $@
	$(AR) rcs $@ $(CORE_OBJS)

install: all
	for p in presets/*.conf; do install -Dm644 $$p $(DESTDIR)$(KITHEME_DATADIR)/$$p; done

uninstall:
	rm -rf $(DESTDIR)$(KITHEME_DATADIR)

clean:
	rm -f core/*.o core/libkitheme.a

.PHONY: all install uninstall clean
