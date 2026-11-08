#!/bin/sh
# Builds build/app/cascadia.deb: the app and its root helper for the iPad's own
# jailbroken iOS 8.4.1.  macOS with Xcode, ldid and dpkg-deb.  The SDK has no
# armv7 slices, so the link goes against stubs/*.tbd (add a symbol there when
# the linker asks); -marm for the Thumb entryoff bug; ldid fake-signs, with no
# entitlements (they panic this jailbreak).
#
# The package carries only our own code.  kloader, iBEC and the boot files come
# from the computer once and are checked against /var/root/cascadia/manifest.
set -e
cd "$(dirname "$0")"
B=../build/app
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CFLAGS="-arch armv7 -miphoneos-version-min=8.0 -isysroot $SDK -marm -Os -Wall -fno-stack-protector"
LD="clang -arch armv7 -miphoneos-version-min=8.0 -nostdlib"

rm -rf "$B"
mkdir -p "$B/pkg/DEBIAN" "$B/pkg/usr/libexec" "$B/pkg/Applications/Cascadia.app/www"
A="$B/pkg/Applications/Cascadia.app"

clang $CFLAGS -c helper.c -o "$B/helper.o"
$LD "$B/helper.o" stubs/libSystem.tbd -o "$B/pkg/usr/libexec/cascadia-helper"
ldid -S "$B/pkg/usr/libexec/cascadia-helper"
chmod 4755 "$B/pkg/usr/libexec/cascadia-helper"

clang $CFLAGS -fblocks -c app.m -o "$B/app.o"
$LD "$B/app.o" stubs/libSystem.tbd stubs/libobjc.tbd stubs/UIKit.tbd -o "$A/Cascadia"
ldid -S "$A/Cascadia"

cp Info.plist "$A/Info.plist"
cp www/index.html "$A/www/index.html"
# Tux from the iPad's own GPU, cut square around the penguin and the mat.
sips -c 768 768 --cropOffset 125 0 tux-source.png --out "$B/icon-square.png" >/dev/null
sips -z 76 76 "$B/icon-square.png" --out "$A/icon.png" >/dev/null
sips -z 152 152 "$B/icon-square.png" --out "$A/icon@2x.png" >/dev/null

cp DEBIAN-control "$B/pkg/DEBIAN/control"
cp DEBIAN-postinst "$B/pkg/DEBIAN/postinst"
chmod 755 "$B/pkg/DEBIAN/postinst"
# iOS 8's dpkg reads gzip, not the zstd or xz of a current dpkg-deb.
dpkg-deb --root-owner-group -Zgzip -b "$B/pkg" "$B/cascadia.deb" >/dev/null
echo "ok: $B/cascadia.deb ($(wc -c < "$B/cascadia.deb" | tr -d ' ') bytes)"
