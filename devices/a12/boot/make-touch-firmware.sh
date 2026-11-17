#!/usr/bin/env bash
# Fetch the N841 touch personality from Apple's 22H374 IPSW and convert it.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$A12/.." && pwd)"
URL='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97953/5F3051CD-5405-4FEF-889E-C3B78E0F376F/iPhone11,8_18.7.10_22H374_Restore.ipsw'
SRC="$A12/boot/extracted/22H374__iPhone11,8/Firmware/N841_Multitouch.im4p"
OUT="$A12/build/touch"
mkdir -p "$OUT"

# The XS Max (A12_BOARD=d331p) runs apple-mt-spi, which takes badapple's op
# list. IPSW=/path/to.ipsw reads a local copy.
if [[ "${A12_BOARD:-n841}" == d331p ]]; then
  URL='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97977/CB8B320D-008B-4FC2-8256-56A1DB86C586/iPhone11,2,iPhone11,4,iPhone11,6_18.7.10_22H374_Restore.ipsw'
  DEST="$A12/boot/extracted/d331p"
  SRC="$DEST/22H374__iPhone11,2_4_6/Firmware/D331p_Multitouch.im4p"
  if [[ ! -s "$SRC" ]]; then
    ipsw extract $([[ -n "${IPSW:-}" ]] || echo --remote) \
      --pattern '^Firmware/D331p_Multitouch\.im4p$' -o "$DEST" "${IPSW:-$URL}"
  fi
  img4 -i "$SRC" -o "$OUT/D331p_Multitouch.plist"
  python3 "$A12/boot/d331p-mtfw.py" "$OUT/D331p_Multitouch.plist" "$OUT/d331p-mtfw.bin"
  exit 0
fi

if [[ ! -s "$SRC" ]]; then
  ipsw extract --remote --pattern '^Firmware/N841_Multitouch\.im4p$' \
    -o "$A12/boot/extracted" "$URL"
fi
[[ -s "$SRC" ]] || { echo "missing $SRC" >&2; exit 1; }
img4 -i "$SRC" -o "$OUT/N841_Multitouch.raw"
python3 "$ROOT/a13/tools/mtfw.py" "$OUT/N841_Multitouch.raw" C1FC2,1 \
  "$OUT/n841-mtfw.bin" "$OUT/n841-mtcal.bin"
echo "Touch firmware ready in $OUT; replace default calibration with this XR's MtCl when available."
