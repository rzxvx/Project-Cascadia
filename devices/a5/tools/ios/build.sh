#!/usr/bin/env bash
# tools/ios/build.sh -- the iPad iOS helpers (tools/ios/*.c) as static armv7
# Mach-O files in build/k93/ios-tools/, unsigned: tools/k93-part.sh pushes
# them to the iPad and signs them there with its own ldid.  Needs docker and
# the cascadia-build image (its Linux ARM cross compiler).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT=build/k93/ios-tools
cd "$ROOT"
mkdir -p "$OUT"
for src in tools/ios/*.c; do
    n=$(basename "$src" .c)
    docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/cascadia" -w /cascadia cascadia-build \
        arm-linux-gnueabihf-gcc -marm -march=armv7-a -mfpu=vfpv3-d16 -O2 -Wall -ffreestanding -fno-builtin \
        -fno-pic -no-pie -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -nostdlib -static -Wl,-Ttext=0x2000 -Wl,--build-id=none -Wl,-e,_start \
        -o "$OUT/$n.elf" "$src" -lgcc
    python3 tools/ios/mkmacho.py "$OUT/$n.elf" "$OUT/$n"
done
