# Unit tests for the shared/ helpers. `make -C shared/tests check`;
# `make -C shared/tests check SAN=1` adds ASan/UBSan.
CC ?= gcc
CFLAGS ?= -O1 -g
CFLAGS += -Wall -Wextra -Wshadow -I..
ifneq ($(SAN),)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

check: test_safefile test_inputscale
	./test_safefile
	./test_inputscale

test_safefile: test_safefile.c ../xis_safefile.c ../xis_safefile.h
	$(CC) $(CFLAGS) -o $@ test_safefile.c ../xis_safefile.c

test_inputscale: test_inputscale.c ../xis_inputscale.c ../xis_inputscale.h
	$(CC) $(CFLAGS) -o $@ test_inputscale.c ../xis_inputscale.c -lxcb -lxcb-randr -lm

clean:
	rm -f test_safefile test_inputscale

.PHONY: check clean
