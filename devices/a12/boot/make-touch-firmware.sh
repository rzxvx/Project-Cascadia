#!/usr/bin/env bash
# Device-specific firmware/calibration preparation; no shared SysCfg fallback.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${1:-}" ]; then export A12_DEVICE="$1"; fi
source "$A12/profile.sh"
OUT="$A12_TOUCH"
mkdir -p "$OUT" "$A12_EXTRACTED/touch"
SRC=$(find "$A12_EXTRACTED/touch" -type f -name "${A12_TOUCH_TAG}_Multitouch.im4p" -print -quit)
if [ -z "$SRC" ]; then
    ipsw extract --remote --pattern "^Firmware/${A12_TOUCH_TAG}_Multitouch\\.im4p$" \
        -o "$A12_EXTRACTED/touch" "$A12_IPSW"
    SRC=$(find "$A12_EXTRACTED/touch" -type f -name "${A12_TOUCH_TAG}_Multitouch.im4p" -print -quit)
fi
[ -n "$SRC" ] || { echo 'missing selected touch firmware' >&2; exit 1; }
RAW="$OUT/${A12_TOUCH_TAG}_Multitouch.raw"
img4 -i "$SRC" -o "$RAW"
case "$A12_DEVICE" in
    xr) python3 "$A12/vendor/a13-tools/mtfw.py" "$RAW" "$A12_TOUCH_PERSONALITY" \
            "$OUT/n841-mtfw.bin" "$OUT/n841-mtcal.bin" ;;
    ipad8)
        python3 "$A12/profile.py" --check-syscfg "$A12_SYSCFG"
        make -C "$A12/tools/makez2fw"
        "$A12/tools/makez2fw/makez2fw" "$A12_TOUCH_PERSONALITY" "$RAW" "$A12_SYSCFG" "$OUT/j171a-mtfw.bin"
        python3 - "$A12" "$A12_SYSCFG" "$OUT" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from profile import validate_syscfg
keys=validate_syscfg(Path(sys.argv[2]),'ipad8')
p=Path(sys.argv[3])/'private';p.mkdir(exist_ok=True)
for key in ('MtCl','FfCl','MtDO','FPCL','MtPc','PBCL','HD2C'):
    dst=p/('j171a-'+key+'.bin')
    if keys.get(key):
        dst.write_bytes(keys[key]);dst.chmod(0o600)
    else:dst.unlink(missing_ok=True)
PY
        ;;
esac
printf '%s\n' "$A12_DEVICE" > "$OUT/device"
echo "$A12_LABEL touch firmware ready in $OUT"
