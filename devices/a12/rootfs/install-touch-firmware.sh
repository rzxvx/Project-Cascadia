#!/usr/bin/env bash
# Keep selected touch firmware available after switch_root (resume/recovery).
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/profile.sh"
ROOT="${1:?usage: install-touch-firmware.sh ROOT}"
test -f "$ROOT/etc/passwd"
case "$A12_DEVICE" in
    xr) files=(n841-mtfw.bin n841-mtcal.bin); mode=644 ;;
    xsmax) files=(d331p-mtfw.bin); mode=644 ;;
    ipad8) files=(j171a-mtfw.bin); mode=600 ;;
esac
# Preflight everything required before modifying the persistent root.
for name in "${files[@]}"; do
    test -s "$A12_TOUCH/$name" || { echo "missing selected touch firmware: $name; run periphery first" >&2; exit 1; }
done
install -d "$ROOT/lib/firmware/apple"
for name in "${files[@]}"; do install -m "$mode" "$A12_TOUCH/$name" "$ROOT/lib/firmware/apple/$name"; done
if [ "$A12_DEVICE" = ipad8 ]; then
    for key in MtCl FfCl MtDO FPCL MtPc PBCL HD2C; do
        p="$A12_TOUCH/private/j171a-$key.bin"
        [ ! -s "$p" ] || install -m 600 "$p" "$ROOT/lib/firmware/apple/"
    done
fi
