#!/bin/sh
# Builds build/mtdump, build/mtlog and build/mtcal: armv7 tools for the iPad's
# own iOS 8.4.1 (jailbroken, OpenSSH).  mtdump prints what iOS knows about the
# digitizer, mtlog streams AppleMultitouchSPI's own trace, mtcal writes the
# panel's calibration to stdout -- see the .c files.  macOS with Xcode only.
#
# Current Xcode still compiles armv7, but its SDK stubs list arm64 only, so
# the link goes against stubs/*.tbd instead: the handful of libSystem, IOKit
# and CoreFoundation symbols mtdump uses, for armv7-ios.  Add a symbol there
# when the linker asks for one.  ldid fake-signs it; the jailbreak takes that.
#
# -marm, not the default Thumb-2: today's linker writes LC_MAIN's entryoff
# without the Thumb bit, so dyld entered a Thumb main() in ARM state and the
# first run died with "Illegal instruction: 4" before printing a line.  ARM
# code needs no bit; calls into Thumb library code interwork through the
# stubs' ldr pc.
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p ../../build
for tool in mtdump mtlog mtcal; do
    OUT=../../build/$tool
    clang -arch armv7 -miphoneos-version-min=8.0 -isysroot "$SDK" -marm -Os -Wall -c $tool.c -o /tmp/$tool.$$.o 2>&1 \
        | grep -v 'incompatible-sysroot' || true
    clang -arch armv7 -miphoneos-version-min=8.0 -nostdlib /tmp/$tool.$$.o \
        stubs/libSystem.tbd stubs/IOKit.tbd stubs/CoreFoundation.tbd -o "$OUT" 2>&1 \
        | grep -v 'incompatible-sysroot' || true
    rm -f /tmp/$tool.$$.o
    ldid -S "$OUT"
    echo "ok: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
done

# mtcal is what ./cascadia mtcal runs on the iPad, from any host -- and only a
# Mac can build it: ld64.lld has armv7 branches and nothing else (its HALF,
# SECTDIFF and VANILLA relocations are FIXMEs, and the stubs), and Apple's ld64
# on Linux needs libtapi, an LLVM build of its own, to read .tbd.  So the
# signed binary is kept in the tree, next to its source, and rebuilt here.
cp ../../build/mtcal prebuilt/mtcal
echo "ok: prebuilt/mtcal updated -- commit it with the source"
