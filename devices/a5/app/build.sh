#!/bin/sh
# Builds build/app/cascadia.deb: the app and its root helper for the iPad's own
# jailbroken iOS 8.4.1.  macOS with Xcode, ldid and dpkg-deb.  The SDK has no
# armv7 slices, so the link goes against stubs/*.tbd (add a symbol there when
# the linker asks); -marm for the Thumb entryoff bug; ldid fake-signs, with no
# entitlements (they panic this jailbreak).
#
# The package carries our own code and BearSSL (MIT).  kloader, iBEC and the boot
# files come from the computer once and are checked against
# /var/root/cascadia/manifest.
#
# BearSSL is not in this repository: the first build downloads it into
# build/cache (bearssl.org, checked by sha256).
set -e
cd "$(dirname "$0")"
B=../build/app
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CFLAGS="-arch armv7 -miphoneos-version-min=8.0 -isysroot $SDK -marm -Os -Wall -fno-stack-protector"
LD="clang -arch armv7 -miphoneos-version-min=8.0 -nostdlib"

BSSL=../build/cache/bearssl-0.6
BSSL_SHA=6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14
BLIB=../build/bearssl-armv7/libbearssl.a
bssl_ok() { [ "$(shasum -a 256 "$BSSL.tar.gz" 2>/dev/null | cut -d' ' -f1)" = "$BSSL_SHA" ]; }
if ! bssl_ok; then
    echo "==> BearSSL 0.6 (once, into build/cache)"
    mkdir -p "$(dirname "$BSSL")"
    curl -fsSL -o "$BSSL.tar.gz.part" https://bearssl.org/bearssl-0.6.tar.gz &&
        mv "$BSSL.tar.gz.part" "$BSSL.tar.gz" || { echo "could not download BearSSL" >&2; exit 1; }
    bssl_ok || { rm -f "$BSSL.tar.gz"; echo "BearSSL's download does not match its sha256" >&2; exit 1; }
    rm -rf "$BSSL"
fi
[ -d "$BSSL/src" ] || tar xzf "$BSSL.tar.gz" -C "$(dirname "$BSSL")"
if [ ! -f "$BLIB" ]; then
    mkdir -p "$(dirname "$BLIB")"
    find "$BSSL/src" -name '*.c' | while read -r f; do
        echo "$f $(dirname "$BLIB")/$(echo "$f" | sed 's|.*/src/||; s|/|_|g; s|\.c$|.o|')"
    done | xargs -P 8 -L 1 sh -c "clang $CFLAGS -w -I$BSSL/inc -I$BSSL/src -c \"\$0\" -o \"\$1\""
    xcrun libtool -static -o "$BLIB" "$(dirname "$BLIB")"/*.o 2>/dev/null
fi

rm -rf "$B"
mkdir -p "$B/pkg/DEBIAN" "$B/pkg/usr/libexec" "$B/pkg/Applications/Cascadia.app/www"
A="$B/pkg/Applications/Cascadia.app"

clang $CFLAGS -c helper.c -o "$B/helper.o"
clang $CFLAGS -c pcless.c -o "$B/pcless.o"
$LD "$B/helper.o" "$B/pcless.o" stubs/libSystem.tbd -o "$B/pkg/usr/libexec/cascadia-helper"
ldid -S "$B/pkg/usr/libexec/cascadia-helper"
chmod 4755 "$B/pkg/usr/libexec/cascadia-helper"

clang $CFLAGS -I"$BSSL/inc" -c fetch.c -o "$B/fetch.o"
$LD "$B/fetch.o" "$BLIB" stubs/libSystem.tbd -o "$B/pkg/usr/libexec/cascadia-fetch"
ldid -S "$B/pkg/usr/libexec/cascadia-fetch"
chmod 755 "$B/pkg/usr/libexec/cascadia-fetch"

clang $CFLAGS -fblocks -c app.m -o "$B/app.o"
$LD "$B/app.o" stubs/libSystem.tbd stubs/libobjc.tbd stubs/UIKit.tbd -o "$A/Cascadia"
ldid -S "$A/Cascadia"

cp Info.plist "$A/Info.plist"
cp www/index.html "$A/www/index.html"
cp "$BSSL/LICENSE.txt" "$A/LICENSE-BearSSL.txt"
# Tux holding a tablet that runs Cascadia (nothing of Apple on it), cut square
# around the head and the tablet.
sips -c 800 800 --cropOffset 30 31 tux-source.png --out "$B/icon-square.png" >/dev/null
sips -z 76 76 "$B/icon-square.png" --out "$A/icon.png" >/dev/null
sips -z 152 152 "$B/icon-square.png" --out "$A/icon@2x.png" >/dev/null

cp DEBIAN-control "$B/pkg/DEBIAN/control"
cp DEBIAN-postinst "$B/pkg/DEBIAN/postinst"
chmod 755 "$B/pkg/DEBIAN/postinst"
# iOS 8's dpkg reads gzip, not the zstd or xz of a current dpkg-deb.
dpkg-deb --root-owner-group -Zgzip -b "$B/pkg" "$B/cascadia.deb" >/dev/null
echo "ok: $B/cascadia.deb ($(wc -c < "$B/cascadia.deb" | tr -d ' ') bytes)"
