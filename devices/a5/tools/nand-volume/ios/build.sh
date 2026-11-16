#!/bin/sh
# Builds cascadia-hfs for the iPad's jailbroken iOS 8.4.1 (armv7, ad-hoc signed
# with ldid), the same way as main's app/build.sh: macOS with Xcode and ldid.
# Output: ../../../build/nand-volume/cascadia-hfs
set -e
cd "$(dirname "$0")"
B=../../../build/nand-volume
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p "$B"
clang -arch armv7 -miphoneos-version-min=8.0 -isysroot "$SDK" -marm -Os -Wall -fno-stack-protector \
    -c hfs-resize.c -o "$B/hfs-resize.o"
clang -arch armv7 -miphoneos-version-min=8.0 -nostdlib "$B/hfs-resize.o" libSystem.tbd -o "$B/cascadia-hfs"
ldid -S "$B/cascadia-hfs"
echo "$B/cascadia-hfs"
