#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../build/payload"
mkdir -p "$OUT"
cpp -nostdinc -I"$HERE/include" -I"$HERE" -undef -D__DTS__ -x assembler-with-cpp \
    "$HERE/t8020-n841.dts" -o "$OUT/t8020-n841.dts.pp"
dtc -I dts -O dtb -o "$OUT/t8020-n841.dtb" "$OUT/t8020-n841.dts.pp"
dtc -I dtb -O dts "$OUT/t8020-n841.dtb" >/dev/null
echo "XR DTB OK: $OUT/t8020-n841.dtb"
