#!/usr/bin/env bash
# Build the board's Linux DTB. A12_BOARD is n841 (XR, default) or d331p (XS Max).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BOARD="${A12_BOARD:-n841}"
OUT="$HERE/../build/payload"
mkdir -p "$OUT"
cpp -nostdinc -I"$HERE/include" -I"$HERE" -undef -D__DTS__ -x assembler-with-cpp \
    "$HERE/t8020-$BOARD.dts" -o "$OUT/t8020-$BOARD.dts.pp"
dtc -I dts -O dtb -o "$OUT/t8020-$BOARD.dtb" "$OUT/t8020-$BOARD.dts.pp"
dtc -I dtb -O dts "$OUT/t8020-$BOARD.dtb" >/dev/null
echo "$BOARD DTB OK: $OUT/t8020-$BOARD.dtb"
