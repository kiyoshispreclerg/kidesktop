#!/bin/sh
# build-deb.sh - builds the kidesktop-core and kidesktop-base .debs into dist/ for the distro it runs on (see
# debian/README.source). Needs debhelper, dpkg-dev and the Build-Depends of
# debian/control installed.
#
# Version: KIDESKTOP_VERSION if set, else the exact git tag (v1.2.3 ->
# 1.2.3), else <VERSION file>+git<commit date>.<commit>; always followed by
# ~<distro><release> (~ubuntu22.04, ~debian12), which sorts below the bare
# version, so a later official package still upgrades over it.
set -eu
cd "$(dirname "$0")/.."

. /etc/os-release
suffix="~${ID}${VERSION_ID:-}"

if [ -n "${KIDESKTOP_VERSION:-}" ]; then
	base="${KIDESKTOP_VERSION#v}"
elif tag=$(git describe --tags --exact-match 2>/dev/null); then
	base="${tag#v}"
else
	base="$(tr -d ' \t\r\n' < VERSION)+git$(git log -1 --format=%cd --date=format:%Y%m%d).$(git rev-parse --short HEAD)"
fi
version="${base}${suffix}"

cat > debian/changelog <<CHANGELOG
kidesktop (${version}) unstable; urgency=medium

  * Build of $(git rev-parse --short HEAD 2>/dev/null || echo unknown).

 -- Kiyoshi Spreclerg <kiyoshi_pip@protonmail.com>  $(date -R)
CHANGELOG

# libvte-dev (GTK2 VTE, kiterm only) does not exist on newer releases such as
# Ubuntu 26.04: drop it from Build-Depends for this build, the suite then
# builds without kiterm (see Makefile).
if ! pkg-config --exists vte 2>/dev/null; then
	cp debian/control debian/control.orig
	trap 'mv -f debian/control.orig debian/control' EXIT
	sed -i '/^[[:space:]]*libvte-dev,\?[[:space:]]*$/d' debian/control
fi

dpkg-buildpackage -b -us -uc

mkdir -p dist
arch=$(dpkg --print-architecture)
for pkg in kidesktop-core kidesktop-base; do
	mv "../${pkg}_${version}_${arch}.deb" dist/
	rm -f "../${pkg}-dbgsym_${version}_${arch}.ddeb" "../${pkg}-dbgsym_${version}_${arch}.deb"
	echo "build-deb: dist/${pkg}_${version}_${arch}.deb"
done
rm -f "../kidesktop_${version}_${arch}.buildinfo" "../kidesktop_${version}_${arch}.changes"
