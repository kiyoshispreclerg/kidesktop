# Unit tests for the shared/ helpers. `make -C shared/tests check`;
# `make -C shared/tests check SAN=1` adds ASan/UBSan.
CC ?= gcc
CFLAGS ?= -O1 -g
CFLAGS += -Wall -Wextra -Wshadow -I..
ifneq ($(SAN),)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

check: test_safefile
	./test_safefile

test_safefile: test_safefile.c ../xis_safefile.c ../xis_safefile.h
	$(CC) $(CFLAGS) -o $@ test_safefile.c ../xis_safefile.c

clean:
	rm -f test_safefile

.PHONY: check clean
