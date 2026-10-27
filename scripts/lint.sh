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

# Core, then base; planned base applications are skipped until they have a
# Makefile.
COMPONENTS="kisession kiwm kicomp xispanel xisserve xisback kitheme kiconf kiconfd xiskeys xismenu xisguard kimemory kistory kimountd"
COMPONENTS="$COMPONENTS kiterm kifiles kifind kiview kishot kitask kipad kipaint"
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
BUILT=""
for c in $COMPONENTS; do
	[ -f "$c/Makefile" ] || continue
	BUILT="$BUILT $c"
	make -s -C "$c" clean >/dev/null 2>&1
	# CFLAGS through the environment, not the command line: the Makefiles
	# append to CFLAGS (LOCALEDIR, ...), which a command-line value would
	# override.
	if ! CFLAGS="$STRICT_CFLAGS" CXXFLAGS="$STRICT_CFLAGS" make -s -C "$c" >/dev/null; then
		fail "build:$c"
	fi
done

echo "==> cppcheck"
if have cppcheck; then
	# arrayIndexOutOfBoundsCond: cppcheck assumes counters can exceed
	# their arrays; every flagged access is already bounds-checked.
	# normalCheckLevelMaxBranches: newer cppcheck's note that big functions
	# get a shallower analysis; it is informational and counts as an error.
	# unknownMacro in a11y.c: GObject type-definition macros; the Qt
	# macros are defined away for kitheme/qt.
	if ! cppcheck -q -j"$(nproc 2>/dev/null || echo 2)" --enable=warning,portability --inline-suppr \
		--suppress=arrayIndexOutOfBoundsCond --suppress=normalCheckLevelMaxBranches \
		--suppress='unknownMacro:*/a11y.c' \
		-DQ_SLOTS= -DQ_SIGNALS=public -DQ_OBJECT= \
		--error-exitcode=1 -D__linux__ -I shared \
		$(echo "$BUILT" | sed 's|\bkicomp\b|kicomp/src|') shared; then
		fail cppcheck
	fi
else
	echo "lint: cppcheck not installed, skipping"
fi

echo "==> shared unit tests (ASan/UBSan)"
make -s -C shared/tests clean >/dev/null 2>&1
if ! CFLAGS="-O1 -g -Werror" make -s -C shared/tests check SAN=1 >/dev/null; then
	fail shared-tests
fi
make -s -C shared/tests clean >/dev/null 2>&1

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
	for f in kiconf/kiconf.desktop.in kiterm/kiterm.desktop.in kifiles/kifiles.desktop.in kishot/kishot.desktop.in kitask/kitask.desktop.in kipaint/kipaint.desktop.in; do
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
