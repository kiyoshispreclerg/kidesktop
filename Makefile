# KiDesktop - build every component
#
# Two groups, matching the two packages (see README, "Building"):
#   core  the desktop itself: session, WM, compositor, panel, launcher, settings...
#   base  the everyday applications: kiterm, kifiles and the ones coming
# ./configure picks which groups a plain `make` builds (--core-only /
# --with-base) and records it in config.mk; without it, both are built.
CORE_COMPONENTS = kisession kiwm kicomp xispanel xisserve xisback \
                  kitheme kiconf kiconfd xiskeys xismenu xisguard kimemory kistory xissound kilock
BASE_COMPONENTS = kiterm kifiles kiview kishot kitask kipad kipaint

WITH_BASE ?= 1
-include config.mk

# Planned base applications have no Makefile yet; they are skipped until
# they do. kiterm also needs VTE; without it the suite builds without it.
HAVE_VTE := $(shell pkg-config --exists vte 2>/dev/null && echo y)
base_ready = $(foreach c,$(BASE_COMPONENTS),$(if $(wildcard $(c)/Makefile),$(c)))
BASE_BUILD = $(if $(HAVE_VTE),$(base_ready),$(filter-out kiterm,$(base_ready)))

COMPONENTS = $(CORE_COMPONENTS) $(BASE_COMPONENTS)
BUILD_COMPONENTS = $(CORE_COMPONENTS) $(if $(filter 1,$(WITH_BASE)),$(BASE_BUILD))

# Full-suite build/install checks the shared dependencies first (see
# ./configure). Building a single component directly, e.g.
# `make -C kiconf install`, skips this and only needs that component's own
# dependencies.
all install: check-deps
	@for c in $(BUILD_COMPONENTS); do \
		echo "==> $$c: $@"; \
		$(MAKE) -C $$c $@ || exit 1; \
	done

# One group on its own, whatever ./configure chose (what the .deb rules use).
core install-core base install-base:
	@for c in $(if $(filter %core,$@),$(CORE_COMPONENTS),$(BASE_BUILD)); do \
		echo "==> $$c: $(if $(filter install-%,$@),install,build)"; \
		$(MAKE) -C $$c $(if $(filter install-%,$@),install) || exit 1; \
	done

uninstall clean:
	@for c in $(COMPONENTS); do \
		[ -f $$c/Makefile ] || continue; \
		echo "==> $$c: $@"; \
		$(MAKE) -C $$c $@ || exit 1; \
	done

check-deps:
	@./configure $(if $(filter 1,$(WITH_BASE)),--with-base,--core-only)

# -Werror build of every component, cppcheck, .po/.desktop/script checks --
# the same thing CI runs (see scripts/lint.sh).
lint:
	@./scripts/lint.sh

# kidesktop-core and kidesktop-base, as <name>_<version>_<arch>.deb for this distro, into dist/ (see
# debian/README.source).
deb:
	@./scripts/build-deb.sh

.PHONY: all install uninstall clean check-deps lint deb core install-core base install-base $(COMPONENTS)
