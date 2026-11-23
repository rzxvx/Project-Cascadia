#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# The iPad 7's GPU firmware (Apple GFX, RTKit on the gfx-kf coprocessor) out
# of the iPadOS 18.7.9 (22H355) kernelcache that tools/firmware.sh fetches.
#
# Where it is (devices/a10/docs/gpu.md, "Firmware"): the kext
# com.apple.AGXFirmwareKextG9PRTBuddy (328.5), __TEXT,__const, right after the
# build string "Release": an ARMv7 / Thumb-2 image, load base 0, 0xa4000
# bytes, followed by a 0x60-byte "fwsg" segment trailer that is not part of
# the image. Apple's binary: it stays in build/, never in git.
#
#   bash tools/gpu/gfx-fw.sh      -> build/firmware/apple/gfx-t8010-22H355.bin
set -euo pipefail
A10="$(cd "$(dirname "$0")/../.." && pwd)"
KC="$A10/build/fwsrc/22H355__iPad7,11/kernelcache.release.iPad7,11_12"
OUT="$A10/build/firmware/apple/gfx-t8010-22H355.bin"
KC_SHA=f0c322154b4927f230b0ed1d06ec445872d6b809c1a0d2acbef36e177c5379e3
OFF=$((0x00e4b87c))   # file offset: kext header 0xe4ae80 + __const 0x9f3 + "\0Release\0"
SIZE=$((0x000a4000))
SHA=264ee9f04618f9ae05015d6033093e72d79f717be3eeb053063407c11d6dec00

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
[ -f "$KC" ] || { echo "error: no kernelcache -- run: ./cascadia a10 firmware" >&2; exit 1; }
[ "$(sha "$KC")" = "$KC_SHA" ] || { echo "error: $KC: SHA-256 mismatch" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
python3 -I - "$KC" "$OUT" "$OFF" "$SIZE" <<'EOF'
import sys
kc, out, off, size = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
d = open(kc, "rb").read()
assert d[off - 8:off] == b"Release\0", "build string not where expected"
assert d[off + size + 0x40:off + size + 0x44] == b"fwsg", "no fwsg trailer after the image"
open(out, "wb").write(d[off:off + size])
EOF
[ "$(sha "$OUT")" = "$SHA" ] || { echo "error: $OUT: SHA-256 mismatch" >&2; exit 1; }
echo "wrote ${OUT#$A10/} ($SIZE bytes, RTKit-2784.140.9)"
