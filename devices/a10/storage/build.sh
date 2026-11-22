#!/usr/bin/env bash
# build.sh - cascadia-storage for iPadOS (arm64), ad-hoc signed by ldid with
# entitlements.plist. Needs Xcode's iPhoneOS SDK and ldid (brew install ldid).
# Output: devices/a10/build/storage/cascadia-storage
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../build/storage"
mkdir -p "$OUT"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 -O2 -Wall -Wextra \
    -o "$OUT/cascadia-storage" "$HERE/cascadia-storage.c"
ldid -S"$HERE/entitlements.plist" "$OUT/cascadia-storage"
echo "storage helper OK: $OUT/cascadia-storage"
