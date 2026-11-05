#!/bin/sh
# install-build-deps.sh - apt-installs debian/control's Build-Depends (the
# one list of what KiDesktop needs to build), plus any extra packages
# given as arguments. Used by CI; also works on a dev machine (as root,
# or via sudo).
#
#   sudo ./scripts/install-build-deps.sh cppcheck desktop-file-utils
set -eu
cd "$(dirname "$0")/.."

deps=$(sed -n '/^Build-Depends:/,/^[A-Za-z-]*:/p' debian/control |
	sed -e '1s/^Build-Depends://' -e '/^[A-Za-z-]*:/d' |
	tr ',' '\n' | sed -e 's/([^)]*)//g' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' |
	sed -e 's/^debhelper-compat$/debhelper/' | grep -v '^$')

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update
# libvte-dev (the GTK2 VTE, only for kiterm) is gone from newer releases such
# as Ubuntu 26.04; the build then skips kiterm, so it is optional here.
optional="libvte-dev"
wanted=""
for d in $deps; do
	case " $optional " in
	*" $d "*)
		if [ -z "$(apt-cache policy "$d" 2>/dev/null | sed -n 's/^  Candidate: //p' | grep -v '(none)')" ]; then
			echo "install-build-deps: $d not available here, skipping (optional)"
			continue
		fi
		;;
	esac
	wanted="$wanted $d"
done
# shellcheck disable=SC2086 # word splitting is the point
$SUDO apt-get install -y --no-install-recommends build-essential dpkg-dev git ca-certificates $wanted "$@"
