#!/bin/bash
# build-deb.sh <version> [arch]   -- e.g. build-deb.sh 1.0.0 amd64
# Packages the arbiter built in ./build as /usr/bin/arbiter with its menu
# entry and icon. Writes the .deb to $OUT (default: this tree).
set -euo pipefail
VER=${1:?usage: build-deb.sh <version> [arch]}
ARCH=${2:-$(dpkg --print-architecture)}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/arbiter
ST=$ROOT/pkg-staging
[ -x "$BIN" ] || { echo "no $BIN -- build first: cmake -B build && cmake --build build"; exit 1; }

rm -rf "$ST"
mkdir -p "$ST/DEBIAN" "$ST/usr/bin" "$ST/usr/share/applications" \
         "$ST/usr/share/icons/hicolor/256x256/apps" "$ST/usr/share/doc/arbiter"
install -m 755 -s "$BIN" "$ST/usr/bin/arbiter"
install -m 644 "$ROOT/packaging/deb/arbiter.desktop" "$ST/usr/share/applications/"
install -m 644 "$ROOT/icons/arbiter.png" "$ST/usr/share/icons/hicolor/256x256/apps/arbiter.png"
install -m 644 "$ROOT/LICENSE" "$ST/usr/share/doc/arbiter/copyright"
install -m 644 "$ROOT/README.md" "$ST/usr/share/doc/arbiter/README.md"
sed -e "s/@VERSION@/$VER/" -e "s/@ARCH@/$ARCH/" "$ROOT/packaging/deb/control.in" > "$ST/DEBIAN/control"
find "$ST" -type d -exec chmod 755 {} +   # never group-writable in a .deb
find "$ST/usr/share" -type f -exec chmod 644 {} +
chmod 644 "$ST/DEBIAN/control"

OUT=${OUT:-$ROOT}/arbiter_${VER}_${ARCH}.deb
dpkg-deb --build --root-owner-group "$ST" "$OUT"
echo "$OUT"
