#!/bin/sh
# build-deb.sh - builds dist/kidesktop_<version>_<arch>.deb for the distro
# it runs on (see debian/README.source). Needs debhelper, dpkg-dev and the
# Build-Depends of debian/control installed.
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

dpkg-buildpackage -b -us -uc

mkdir -p dist
arch=$(dpkg --print-architecture)
mv "../kidesktop_${version}_${arch}.deb" dist/
rm -f "../kidesktop_${version}_${arch}.buildinfo" "../kidesktop_${version}_${arch}.changes" \
	"../kidesktop-dbgsym_${version}_${arch}.ddeb" "../kidesktop-dbgsym_${version}_${arch}.deb"
echo "build-deb: dist/kidesktop_${version}_${arch}.deb"
