# KiDesktop - build every component
COMPONENTS = kisession kiwm kicomp xispanel xisserve xisnotif xisback \
             kiconf kiconfd xiskeys xismenu xisguard

all install uninstall clean:
	@for c in $(COMPONENTS); do \
		echo "==> $$c: $@"; \
		$(MAKE) -C $$c $@ || exit 1; \
	done

.PHONY: all install uninstall clean $(COMPONENTS)
