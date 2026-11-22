#!/usr/bin/env bash
# firmware.sh - the iPad 7's firmware that comes from Apple's IPSW, read from
# Apple's CDN (partial reads: the kernelcache, not the whole IPSW).
#
# The reference build is iPadOS 18.7.9 (22H355, one IPSW for iPad7,11 and
# iPad7,12), the one the kernel's drivers were validated with (Pauli1Go's
# HoolockLinux-linux-firmware, ipad7.md). Every source and output is checked
# against the sizes and SHA-256 sums given there.
#
#   apple/t8010-smartio.bin   the SmartIO coprocessor (SPI DMA for the touch
#                             controller): AppleT8010SmartIO's __DATA.__firmware
#
# Output: devices/a10/build/firmware/<path under /lib/firmware>; pack puts
# the tree into the initramfs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
SRC="$A10/build/fwsrc"
FW="$A10/build/firmware"
mkdir -p "$SRC" "$FW/apple"

BUILD=22H355
KC="$SRC/${BUILD}__iPad7,11/kernelcache.release.iPad7,11_12"
KC_SIZE=48529408
KC_SHA=f0c322154b4927f230b0ed1d06ec445872d6b809c1a0d2acbef36e177c5379e3
SIO_OFF=$((0x02a46000))
SIO_SIZE=$((0x00088060))
SIO_SHA=71d274a106f5912ed33552ba75ae76cc5081fc7063935ec938f9cb503df38199

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
check() {   # file size sha
    [ "$(wc -c < "$1" | tr -d ' ')" = "$2" ] || { echo "error: $1: size is not $2" >&2; exit 1; }
    [ "$(sha "$1")" = "$3" ] || { echo "error: $1: SHA-256 mismatch" >&2; exit 1; }
}

if [ ! -f "$KC" ]; then
    echo "==> kernelcache of iPadOS 18.7.9 ($BUILD) from Apple's CDN"
    ( cd "$SRC" && ipsw download ipsw --build "$BUILD" -d iPad7,11 -y --kernel >/dev/null )
fi
check "$KC" "$KC_SIZE" "$KC_SHA"

echo "==> apple/t8010-smartio.bin"
dd if="$KC" of="$FW/apple/t8010-smartio.bin" bs=4096 \
    skip=$((SIO_OFF / 4096)) count=$(((SIO_SIZE + 4095) / 4096)) 2>/dev/null
truncate -s "$SIO_SIZE" "$FW/apple/t8010-smartio.bin"
check "$FW/apple/t8010-smartio.bin" "$SIO_SIZE" "$SIO_SHA"

echo "firmware OK: $FW"
