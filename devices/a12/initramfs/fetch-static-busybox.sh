#!/usr/bin/env bash
# Extract only: never replace the host's dynamic BusyBox package.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$A12/build/tools/busybox-static"
SHA=968d1aa8f579fa1ac59c26afa365454369e13cf29848e6400b50028fed0ffda0
URL='https://http.kali.org/kali/pool/main/b/busybox/busybox-static_1.38.0-3+b1_arm64.deb'
mkdir -p "$CACHE"
if ! echo "$SHA  $CACHE/package.deb" | sha256sum -c --status; then
    curl --fail --location --retry 3 "$URL" -o "$CACHE/package.deb.part"
    echo "$SHA  $CACHE/package.deb.part" | sha256sum -c -
    mv "$CACHE/package.deb.part" "$CACHE/package.deb"
fi
dpkg-deb -x "$CACHE/package.deb" "$CACHE/extracted"
BIN="$CACHE/extracted/usr/bin/busybox"
file "$BIN" | grep -q 'ARM aarch64.*statically linked'
! readelf -l "$BIN" | grep -q INTERP
! readelf -d "$BIN" | grep -q NEEDED
"$BIN" true
for applet in sh sleep cat ls head ifconfig grep udhcpd touch cut; do
    "$BIN" --list | grep -qx "$applet"
done
echo "$BIN"
