#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build gpucap for the jailbroken iPadOS (arm64, ad-hoc signed by ldid with
# entitlements.plist; never a developer certificate).
#   bash tools/gpu/ios/build.sh   -> build/bin/gpucap
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../../build/bin"
mkdir -p "$OUT"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra -fobjc-arc \
    -framework Foundation -framework Metal -o "$OUT/gpucap" "$HERE/gpucap.m"
ldid -S"$HERE/entitlements.plist" "$OUT/gpucap"
echo "gpucap OK: build/bin/gpucap"
