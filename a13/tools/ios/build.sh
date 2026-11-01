#!/bin/sh
# Builds a13/build/ios/kmem: the iOS-side tool for the GPU work (gpu.md, G1),
# for the iPhone's iOS 16.4 with Dopamine.  macOS with Xcode + ldid only.
# Install:  scp a13/build/ios/kmem iphone11-ios:/var/jb/usr/local/bin/
set -e
cd "$(dirname "$0")"
OUT=../../build/ios
mkdir -p $OUT
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 -isysroot "$SDK" \
    -Os -Wall -Wextra -Wno-unused-parameter \
    -framework CoreFoundation -framework IOKit kmem.c -o $OUT/kmem
ldid -Skmem.entitlements $OUT/kmem
echo "ok: $OUT/kmem ($(wc -c < $OUT/kmem | tr -d ' ') bytes)"
