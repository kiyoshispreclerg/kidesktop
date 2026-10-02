#!/bin/sh
# lint.sh - what `make lint` (and CI) checks. Run from the source root.
#
#   1. every component builds with -Werror (clean rebuild, in tree)
#   2. cppcheck finds nothing at warning level
#   3. every .po compiles with msgfmt --check
#   4. the .desktop templates validate
#   5. shell and Python scripts parse
#
# Steps whose tool isn't installed are skipped with a note, so the script
# also runs on a machine with only the build dependencies.
set -u

COMPONENTS="kisession kiwm kicomp xispanel xisserve xisback kiconf kiconfd xiskeys xismenu xisguard kimemory kistory"
STRICT_CFLAGS="-Wall -Wextra -O2 -Werror"
FAILED=""

fail() {
	FAILED="$FAILED $1"
	echo "lint: FAILED: $1"
}

have() {
	command -v "$1" >/dev/null 2>&1
}

echo "==> build with -Werror"
for c in $COMPONENTS; do
	make -s -C "$c" clean >/dev/null 2>&1
	# CFLAGS through the environment, not the command line: the Makefiles
	# append to CFLAGS (LOCALEDIR, ...), which a command-line value would
	# override.
	if ! CFLAGS="$STRICT_CFLAGS" make -s -C "$c" >/dev/null; then
		fail "build:$c"
	fi
done

echo "==> cppcheck"
if have cppcheck; then
	# arrayIndexOutOfBoundsCond: cppcheck assumes counters can exceed
	# their arrays; every flagged access is already bounds-checked.
	# unknownMacro in a11y.c: GObject type-definition macros.
	if ! cppcheck -q -j"$(nproc 2>/dev/null || echo 2)" --enable=warning,portability --inline-suppr \
		--suppress=arrayIndexOutOfBoundsCond --suppress='unknownMacro:*/a11y.c' \
		--error-exitcode=1 -D__linux__ -I shared \
		kisession kiwm kicomp/src xispanel xisserve xisback kiconf kiconfd xiskeys xismenu xisguard \
		kimemory kistory shared; then
		fail cppcheck
	fi
else
	echo "lint: cppcheck not installed, skipping"
fi

echo "==> translations"
for po in */po/*.po; do
	[ -f "$po" ] || continue
	msgfmt --check -o /dev/null "$po" || fail "msgfmt:$po"
done

echo "==> desktop files"
if have desktop-file-validate; then
	tmp=$(mktemp -d)
	# Application entries only: kisession.desktop is an xsessions file,
	# whose DesktopNames= key (read by GDM/LightDM) the validator rejects.
	for f in kiconf/kiconf.desktop.in; do
		out="$tmp/$(basename "$f" .in)"
		sed 's|@BINDIR@|/usr/bin|' "$f" > "$out"
		desktop-file-validate "$out" || fail "desktop:$f"
	done
	rm -rf "$tmp"
else
	echo "lint: desktop-file-validate not installed, skipping"
fi

echo "==> scripts"
for s in configure scripts/*.sh; do
	sh -n "$s" || fail "sh:$s"
done
for p in $(git ls-files '*.py' 2>/dev/null); do
	python3 -m py_compile "$p" 2>/dev/null || fail "python:$p"
done

if [ -n "$FAILED" ]; then
	echo
	echo "lint failed:$FAILED"
	exit 1
fi
echo
echo "lint: all checks passed"
