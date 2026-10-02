# KiDesktop - build every component
COMPONENTS = kisession kiwm kicomp xispanel xisserve xisback \
             kiconf kiconfd xiskeys xismenu xisguard kimemory kistory

# Full-suite build/install checks the shared dependencies first (see
# ./configure). Building a single component directly, e.g.
# `make -C kiconf install`, skips this and only needs that component's own
# dependencies.
all install: check-deps
	@for c in $(COMPONENTS); do \
		echo "==> $$c: $@"; \
		$(MAKE) -C $$c $@ || exit 1; \
	done

uninstall clean:
	@for c in $(COMPONENTS); do \
		echo "==> $$c: $@"; \
		$(MAKE) -C $$c $@ || exit 1; \
	done

check-deps:
	@./configure

# -Werror build of every component, cppcheck, .po/.desktop/script checks --
# the same thing CI runs (see scripts/lint.sh).
lint:
	@./scripts/lint.sh

.PHONY: all install uninstall clean check-deps lint $(COMPONENTS)
