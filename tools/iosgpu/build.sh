#!/bin/sh
# Builds build/gltrace: an armv7 OpenGL ES 2 tool for the iPad's own jailbroken
# iOS 8.4.1.  It draws a triangle offscreen and fishhooks the IOKit calls the
# GL driver makes into the kernel, to reverse the SGX543 command stream.
# macOS with Xcode only; see gltrace.m.  Same stub/ldid approach as
# tools/mtdump: the SDK has no armv7 slices, so the link goes against
# stubs/*.tbd (add a symbol there when the linker asks), -marm for the Thumb
# entryoff bug, ldid fake-signs it (no entitlements -- entitlements panic this
# jailbreak; see tools/mtdump/kmemprobe.c).
set -e
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p ../../build
CFLAGS="-arch armv7 -miphoneos-version-min=8.0 -isysroot $SDK -marm -Os -Wall"
clang $CFLAGS -c fishhook.c -o /tmp/fishhook.$$.o
clang $CFLAGS -c gltrace.m  -o /tmp/gltrace.$$.o
clang -arch armv7 -miphoneos-version-min=8.0 -nostdlib /tmp/gltrace.$$.o /tmp/fishhook.$$.o \
    stubs/libSystem.tbd stubs/libobjc.tbd stubs/OpenGLES.tbd -o ../../build/gltrace
rm -f /tmp/fishhook.$$.o /tmp/gltrace.$$.o
ldid -S ../../build/gltrace
echo "ok: ../../build/gltrace ($(wc -c < ../../build/gltrace | tr -d ' ') bytes)"
