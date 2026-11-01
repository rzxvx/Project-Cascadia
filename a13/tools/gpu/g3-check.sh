#!/bin/sh
# G3 (research/gpu.md): did our boot (sig-patched iBSS in Recovery -> bootx)
# load the GPU firmware the way iOS's iBoot does?  Runs on the phone under
# Linux, as root; writes /tmp/g3/: the live ADT and the firmware's __TEXT.
#   g3-check.sh [TEXT_PHYS]   (default: where iOS's iBoot put it; when iBoot
#                              loaded it for us, take gfx-asc segment 0 from
#                              the ADT first: g3-compare.py)
#
# Reads only __TEXT (0x8002c0000 +0x4c000), which iOS reads safely.  Never
# the firmware's __DATA or the GPU's regions: on iOS one read of those reset
# the phone (gpu.md, G1/G2).
set -e
TEXT=${1:-0x8002c0000}
TEXT_PAGES=19   # 0x4c000 / 16 KiB
mkdir -p /tmp/g3
cat /dev/mtd1 > /tmp/g3/adt.bin
echo "live ADT: $(wc -c < /tmp/g3/adt.bin) bytes"
dd if=/dev/mem of=/tmp/g3/gfx-text.bin bs=16384 skip=$((TEXT / 16384)) count=$TEXT_PAGES 2>/dev/null &&
    echo "__TEXT: $(wc -c < /tmp/g3/gfx-text.bin) bytes" ||
    echo "__TEXT: /dev/mem refused"
head -c 16 /tmp/g3/gfx-text.bin | od -An -tx1
