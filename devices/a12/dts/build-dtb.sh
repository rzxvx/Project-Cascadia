#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "${1:-}" ]; then export A12_DEVICE="$1"; fi
source "$HERE/../profile.sh"
OUT="$A12_PAYLOAD"
BOARD="$A12_BOARD"
case "$BOARD" in
  n841) NAME=t8020-n841; LABEL=XR ;;
  j171a) NAME=t8020-j171a; LABEL='iPad 8' ;;
  *) echo "usage: $0 [n841|j171a]" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
cpp -nostdinc -I"$HERE/include" -I"$HERE" -undef -D__DTS__ -x assembler-with-cpp \
    "$HERE/$NAME.dts" -o "$OUT/$NAME.dts.pp"
dtc -I dts -O dtb -o "$OUT/$NAME.dtb" "$OUT/$NAME.dts.pp"
dtc -I dtb -O dts "$OUT/$NAME.dtb" >/dev/null
echo "$LABEL DTB OK: $OUT/$NAME.dtb"
